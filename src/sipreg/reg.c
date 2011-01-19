/**
 * @file reg.c  SIP Registration
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <re_types.h>
#include <re_mem.h>
#include <re_mbuf.h>
#include <re_sa.h>
#include <re_list.h>
#include <re_hash.h>
#include <re_fmt.h>
#include <re_uri.h>
#include <re_tmr.h>
#include <re_sip.h>
#include <re_sipreg.h>


enum {
	DEFAULT_EXPIRES = 3600,
	FAIL_WAIT = 60 * 1000,
};


struct sipreg {
	struct sip_loopstate ls;
	struct sa laddr;
	struct tmr tmr;
	struct sip *sip;
	struct sip_request *req;
	struct sip_dialog *dlg;
	struct sip_auth *auth;
	struct mbuf *hdrs;
	char *cuser;
	sip_resp_h *resph;
	void *arg;
	uint32_t expires;
	uint32_t wait;
	enum sip_transp tp;
	bool registered;
	bool terminated;
	char *params;
};


static int request(struct sipreg *reg, bool reset_ls);


static void dummy_handler(int err, const struct sip_msg *msg, void *arg)
{
	(void)err;
	(void)msg;
	(void)arg;
}


static void destructor(void *arg)
{
	struct sipreg *reg = arg;

	tmr_cancel(&reg->tmr);

	if (!reg->terminated) {

		reg->resph = dummy_handler;
		reg->terminated = true;

		if (reg->req) {
			mem_ref(reg);
			return;
		}

		if (reg->registered && !request(reg, true)) {
			mem_ref(reg);
			return;
		}
	}

	mem_deref(reg->dlg);
	mem_deref(reg->auth);
	mem_deref(reg->cuser);
	mem_deref(reg->sip);
	mem_deref(reg->hdrs);
	mem_deref(reg->params);
}


static void tmr_handler(void *arg)
{
	struct sipreg *reg = arg;
	int err;

	err = request(reg, true);
	if (err) {
		tmr_start(&reg->tmr, FAIL_WAIT, tmr_handler, reg);
		reg->resph(err, NULL, reg->arg);
	}
}


static bool contact_handler(const struct sip_hdr *hdr,
			    const struct sip_msg *msg, void *arg)
{
	struct sipreg *reg = arg;
	struct sip_addr c;
	struct pl pval;
	char uri[256];

	if (sip_addr_decode(&c, &hdr->val))
		return false;

	if (re_snprintf(uri, sizeof(uri), "sip:%s@%J%s", reg->cuser,
			&reg->laddr, sip_transp_param(reg->tp)) < 0)
		return false;

	if (pl_strcmp(&c.auri, uri))
		return false;

	if (!sip_param_decode(&c.params, "expires", &pval)) {
	        reg->wait = pl_u32(&pval);
	}
	else if (pl_isset(&msg->expires))
	        reg->wait = pl_u32(&msg->expires);
	else
	        reg->wait = DEFAULT_EXPIRES;

	return true;
}


static void response_handler(int err, const struct sip_msg *msg, void *arg)
{
	const struct sip_hdr *minexp;
	struct sipreg *reg = arg;

	reg->wait = FAIL_WAIT;

	if (err || sip_request_loops(&reg->ls, msg->scode))
		goto out;

	if (msg->scode < 200) {
		return;
	}
	else if (msg->scode < 300) {
		reg->registered = true;
		reg->wait = reg->expires;
		sip_msg_hdr_apply(msg, true, SIP_HDR_CONTACT, contact_handler,
				  reg);
		reg->wait *= 900;
	}
	else {
		if (reg->terminated && !reg->registered)
			goto out;

		switch (msg->scode) {

		case 401:
		case 407:
			err = sip_auth_authenticate(reg->auth, msg);
			if (err) {
				err = (err == EAUTH) ? 0 : err;
				break;
			}

			err = request(reg, false);
			if (err)
				break;

			return;

		case 403:
			sip_auth_reset(reg->auth);
			break;

		case 423:
			minexp = sip_msg_hdr(msg, SIP_HDR_MIN_EXPIRES);
			if (!minexp || !pl_u32(&minexp->val) || !reg->expires)
				break;

			reg->expires = pl_u32(&minexp->val);

			err = request(reg, false);
			if (err)
				break;

			return;
		}
	}

 out:
	if (!reg->expires) {
		mem_deref(reg);
	}
	else if (reg->terminated) {
		if (!reg->registered || request(reg, true))
			mem_deref(reg);
	}
	else {
		tmr_start(&reg->tmr, reg->wait, tmr_handler, reg);
		reg->resph(err, msg, reg->arg);
	}
}


static int send_handler(enum sip_transp tp, const struct sa *src,
			const struct sa *dst, struct mbuf *mb, void *arg)
{
	struct sipreg *reg = arg;
	(void)dst;

	if (reg->expires > 0) {
		reg->laddr = *src;
		reg->tp = tp;
	}

	return mbuf_printf(mb, "Contact: <sip:%s@%J%s>;expires=%u%s%s\r\n",
			   reg->cuser, &reg->laddr, sip_transp_param(reg->tp),
			   reg->expires,
			   reg->params ? ";" : "",
			   reg->params ? reg->params : "");
}


static int request(struct sipreg *reg, bool reset_ls)
{
	if (reg->terminated)
		reg->expires = 0;

	if (reset_ls)
		sip_loopstate_reset(&reg->ls);

	return sip_drequestf(&reg->req, reg->sip, true, "REGISTER", reg->dlg,
			     0, reg->auth, send_handler, response_handler, reg,
			     "%b"
			     "Content-Length: 0\r\n"
			     "\r\n",
			     reg->hdrs ? mbuf_buf(reg->hdrs) : NULL,
			     reg->hdrs ? mbuf_get_left(reg->hdrs) : 0);
}


int sipreg_register(struct sipreg **regp, struct sip *sip, const char *reg_uri,
		    const char *to_uri, const char *from_uri, uint32_t expires,
		    const char *cuser, const char *routev[], uint32_t routec,
		    sip_auth_h *authh, void *aarg, bool aref,
		    sip_resp_h *resph, void *arg,
		    const char *params, const char *fmt, ...)
{
	struct sipreg *reg;
	int err;

	if (!regp || !sip || !reg_uri || !to_uri || !from_uri ||
	    !expires || !cuser)
		return EINVAL;

	reg = mem_zalloc(sizeof(*reg), destructor);
	if (!reg)
		return ENOMEM;

	err = sip_dialog_alloc(&reg->dlg, reg_uri, to_uri, NULL, from_uri,
			       routev, routec);
	if (err)
		goto out;

	err = sip_auth_alloc(&reg->auth, authh, aarg, aref);
	if (err)
		goto out;

	err = str_dup(&reg->cuser, cuser);
	if (params)
		err |= str_dup(&reg->params, params);
	if (err)
		goto out;

	/* Custom SIP headers */
	if (fmt) {
		va_list ap;

		reg->hdrs = mbuf_alloc(256);
		if (!reg->hdrs) {
			err = ENOMEM;
			goto out;
		}

		va_start(ap, fmt);
		err = mbuf_vprintf(reg->hdrs, fmt, ap);
		reg->hdrs->pos = 0;
		va_end(ap);

		if (err)
			goto out;
	}

	reg->sip     = mem_ref(sip);
	reg->expires = expires;
	reg->resph   = resph ? resph : dummy_handler;
	reg->arg     = arg;

	err = request(reg, true);
	if (err)
		goto out;

 out:
	if (err)
		mem_deref(reg);
	else
		*regp = reg;

	return err;
}