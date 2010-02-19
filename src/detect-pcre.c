/* PCRE part of the detection engine. */

#include "suricata-common.h"
#include "debug.h"
#include "decode.h"
#include "detect.h"

#include "pkt-var.h"
#include "flow-var.h"
#include "flow-alert-sid.h"

#include "detect-pcre.h"

#include "detect-parse.h"
#include "detect-engine.h"
#include "detect-engine-mpm.h"

#include "util-var-name.h"
#include "util-debug.h"
#include "util-unittest.h"
#include "util-print.h"
#include "util-pool.h"

#include "conf.h"
#include "app-layer-htp.h"
#include "stream.h"
#include "stream-tcp.h"
#include "stream-tcp-private.h"
#include "stream-tcp-reassemble.h"
#include "app-layer-protos.h"
#include "app-layer-parser.h"
#include "app-layer-htp.h"

#include <htp/htp.h>
#include "stream.h"


#define PARSE_CAPTURE_REGEX "\\(\\?P\\<([A-z]+)\\_([A-z0-9_]+)\\>"
#define PARSE_REGEX         "(?<!\\\\)/(.*)(?<!\\\\)/([^\"]*)"

#define DEFAULT_MATCH_LIMIT 10000000
#define DEFAULT_MATCH_LIMIT_RECURSION 10000000

#define MATCH_LIMIT_DEFAULT 1500

static int pcre_match_limit = 0;
static int pcre_match_limit_recursion = 0;

static pcre *parse_regex;
static pcre_extra *parse_regex_study;
static pcre *parse_capture_regex;
static pcre_extra *parse_capture_regex_study;

uint8_t pcre_need_htp_request_body = 0;

int DetectPcreMatch (ThreadVars *, DetectEngineThreadCtx *, Packet *, Signature *, SigMatch *);
int DetectPcreALMatch(ThreadVars *t, DetectEngineThreadCtx *det_ctx, Flow *f, uint8_t flags, void *state, Signature *s, SigMatch *m);
int DetectPcreSetup (DetectEngineCtx *, Signature *, SigMatch *, char *);
void DetectPcreFree(void *);
void DetectPcreRegisterTests(void);

void DetectPcreRegister (void) {
    sigmatch_table[DETECT_PCRE].name = "pcre";
    sigmatch_table[DETECT_PCRE].Match = DetectPcreMatch;
    sigmatch_table[DETECT_PCRE].AppLayerMatch = NULL;
    sigmatch_table[DETECT_PCRE].alproto = ALPROTO_HTTP;
    sigmatch_table[DETECT_PCRE].Setup = DetectPcreSetup;
    sigmatch_table[DETECT_PCRE].Free  = DetectPcreFree;
    sigmatch_table[DETECT_PCRE].RegisterTests  = DetectPcreRegisterTests;

    sigmatch_table[DETECT_PCRE].flags |= SIGMATCH_PAYLOAD;

    /* register a separate sm type for the httpbody stuff
     * because then we don't need to figure out if we need
     * the match or AppLayerMatch function in Detect */
    sigmatch_table[DETECT_PCRE_HTTPBODY].name = "__pcre_http_body__"; /* not a real keyword */
    sigmatch_table[DETECT_PCRE_HTTPBODY].Match = NULL;
    sigmatch_table[DETECT_PCRE_HTTPBODY].AppLayerMatch = DetectPcreALMatch;
    sigmatch_table[DETECT_PCRE_HTTPBODY].alproto = ALPROTO_HTTP;
    sigmatch_table[DETECT_PCRE_HTTPBODY].Setup = NULL;
    sigmatch_table[DETECT_PCRE_HTTPBODY].Free  = DetectPcreFree;
    sigmatch_table[DETECT_PCRE_HTTPBODY].RegisterTests  = NULL;

    sigmatch_table[DETECT_PCRE_HTTPBODY].flags |= SIGMATCH_PAYLOAD;

    const char *eb;
    int eo;
    int opts = 0;
    intmax_t val = 0;

    if (!ConfGetInt("pcre.match-limit", &val)) {
        pcre_match_limit = DEFAULT_MATCH_LIMIT;
    }
    else    {
        pcre_match_limit = val;
    }

    val = 0;

    if (!ConfGetInt("pcre.match-limit-recursion", &val)) {
        pcre_match_limit_recursion = DEFAULT_MATCH_LIMIT_RECURSION;
    }
    else    {
        pcre_match_limit_recursion = val;
    }

    parse_regex = pcre_compile(PARSE_REGEX, opts, &eb, &eo, NULL);
    if(parse_regex == NULL)
    {
        SCLogError(SC_ERR_PCRE_COMPILE, "pcre compile of \"%s\" failed at offset %" PRId32 ": %s", PARSE_REGEX, eo, eb);
        goto error;
    }

    parse_regex_study = pcre_study(parse_regex, 0, &eb);
    if(eb != NULL)
    {
        SCLogError(SC_ERR_PCRE_STUDY, "pcre study failed: %s", eb);
        goto error;
    }

    opts |= PCRE_UNGREEDY; /* pkt_http_ua should be pkt, http_ua, for this reason the UNGREEDY */
    parse_capture_regex = pcre_compile(PARSE_CAPTURE_REGEX, opts, &eb, &eo, NULL);
    if(parse_capture_regex == NULL)
    {
        SCLogError(SC_ERR_PCRE_COMPILE, "pcre compile of \"%s\" failed at offset %" PRId32 ": %s", PARSE_CAPTURE_REGEX, eo, eb);
        goto error;
    }

    parse_capture_regex_study = pcre_study(parse_capture_regex, 0, &eb);
    if(eb != NULL)
    {
        SCLogError(SC_ERR_PCRE_STUDY, "pcre study failed: %s", eb);
        goto error;
    }
    return;

error:
    /* XXX */
    return;
}

/**
 * \brief match the specified pcre at http body, requesting it from htp/L7
 *
 * \param t pointer to thread vars
 * \param det_ctx pointer to the pattern matcher thread
 * \param p pointer to the current packet
 * \param m pointer to the sigmatch that we will cast into DetectPcreData
 *
 * \retval int 0 no match; 1 match
 */
int DetectPcreALMatch(ThreadVars *t, DetectEngineThreadCtx *det_ctx, Flow *f,
                      uint8_t flags, void *state, Signature *s, SigMatch *m)
{
#define MAX_SUBSTRINGS 30
    SCEnter();
    int ret = 0;
    int pcreret = 0;
    int ov[MAX_SUBSTRINGS];

    DetectPcreData *pe = (DetectPcreData *)m->ctx;
    if ( !(pe->flags & DETECT_PCRE_HTTP_BODY_AL))
        SCReturnInt(0);

    SCMutexLock(&f->m);

    /** If enabled http body inspection
      * TODO: Add more HTTP options here if needed
      */
    HtpState *htp_state = (HtpState *)state;
    if (htp_state == NULL) {
        SCLogDebug("No htp state, no match at http body data");
        goto unlock;
    } else if (htp_state->body.nchunks == 0) {
        SCLogDebug("No body data to inspect");
        goto unlock;
    } else {
        pcreret = 0;
        int wspace[255];
        int flags = PCRE_PARTIAL;

        BodyChunk *cur = htp_state->body.first;
        if (cur == NULL) {
            SCLogDebug("No body chunks to inspect");
            goto unlock;
        }
        htp_state->body.pcre_flags |= HTP_PCRE_DONE;

        while (cur != NULL) {
            if (SCLogDebugEnabled()) {
                printf("\n");
                PrintRawUriFp(stdout, (uint8_t*)cur->data, cur->len);
                printf("\n");
            }
            pcreret = pcre_dfa_exec(pe->re, NULL, (char*)cur->data, cur->len, 0,
                                    flags|PCRE_DFA_SHORTEST, ov, MAX_SUBSTRINGS,
                                    wspace, MAX_SUBSTRINGS);
            cur = cur->next;

            SCLogDebug("Pcre Ret %d", pcreret);
            switch (pcreret) {
                case PCRE_ERROR_PARTIAL:
                    /* make pcre to use the working space of the last partial
                     * match, (match over multiple chunks)
                     */
                    SCLogDebug("partial match");
                    flags |= PCRE_DFA_RESTART;
                    htp_state->body.pcre_flags |= HTP_PCRE_HAS_MATCH;
                break;
                case PCRE_ERROR_NOMATCH:
                    SCLogDebug("no match");
                    flags = PCRE_PARTIAL;
                break;
                case 0:
                    SCLogDebug("Perfect Match!");
                    ret = 1;
                    goto unlock;
                break;
                default:
                    if (pcreret > 0) {
                        SCLogDebug("Match with captured data");
                        ret = 1;
                    } else {
                        SCLogDebug("No match, pcre failed");
                        ret = 0;
                    }
                    goto unlock;
            }
        }
    }

unlock:
    SCMutexUnlock(&f->m);
    SCReturnInt(ret ^ pe->negate);
}

/**
 * \brief DetectPcreMatch will try to match a regex on a single packet;
 *        DetectPcreALMatch is used if we parse the option 'P'
 *
 * \param t pointer to the threadvars structure
 * \param t pointer to the threadvars structure
 *
 * \retval 1: match ; 0 No Match; -1: error
 */
int DetectPcreMatch (ThreadVars *t, DetectEngineThreadCtx *det_ctx, Packet *p,
                     Signature *s, SigMatch *m)
{
    SCEnter();
#define MAX_SUBSTRINGS 30
    int ret = 0;
    int ov[MAX_SUBSTRINGS];
    uint8_t *ptr = NULL;
    uint16_t len = 0;

    if (p->payload_len == 0)
        SCReturnInt(0);

    DetectPcreData *pe = (DetectPcreData *)m->ctx;

    /* If we want to inspect the http body, we will use HTP L7 parser */
    if (pe->flags & DETECT_PCRE_HTTP_BODY_AL)
        SCReturnInt(0);

    if (s->flags & SIG_FLAG_RECURSIVE) {
        ptr = det_ctx->pkt_ptr ? det_ctx->pkt_ptr : p->payload;
        len = p->payload_len - det_ctx->pkt_off;
    } else if (pe->flags & DETECT_PCRE_RELATIVE) {
        ptr = det_ctx->pkt_ptr;
        len = p->payload_len - det_ctx->pkt_off;
        if (ptr == NULL || len == 0)
            SCReturnInt(0);
    } else {
        ptr = p->payload;
        len = p->payload_len;
    }

    //printf("DetectPcre: ptr %p, len %" PRIu32 "\n", ptr, len);

    ret = pcre_exec(pe->re, pe->sd, (char *)ptr, len, 0, 0, ov, MAX_SUBSTRINGS);
    SCLogDebug("ret %d (negating %s)", ret, pe->negate ? "set" : "not set");

    if (ret == PCRE_ERROR_NOMATCH) {
        if (pe->negate == 1) {
            /* regex didn't match with negate option means we consider it a match */
            ret = 1;
        } else {
            ret = 0;
        }
    } else if (ret >= 0) {
        if (pe->negate == 1) {
            /* regex matched but we're negated, so not considering it a match */
            ret = 0;
        } else {
            /* regex matched and we're not negated, considering it a match */
            if (ret > 1 && pe->capidx != 0) {
                const char *str_ptr;
                ret = pcre_get_substring((char *)ptr, ov, MAX_SUBSTRINGS, 1, &str_ptr);
                if (ret) {
                    if (strcmp(pe->capname,"http_uri") == 0) {
                        p->http_uri.raw[det_ctx->pkt_cnt] = (uint8_t *)str_ptr;
                        p->http_uri.raw_size[det_ctx->pkt_cnt] = ret;
                        p->http_uri.cnt = det_ctx->pkt_cnt + 1;

                        /* count how many uri's we handle for stats */
                        det_ctx->uris++;

                        //printf("DetectPcre: URI det_ctx->sgh %p, det_ctx->mcu %p\n", det_ctx->sgh, det_ctx->mcu);
                        //PrintRawUriFp(stdout,p->http_uri.raw[det_ctx->pkt_cnt],p->http_uri.raw_size[det_ctx->pkt_cnt]);
                        //printf(" (pkt_cnt %" PRIu32 ", mcu %p)\n", det_ctx->pkt_cnt, det_ctx->mcu);
#if 0
                        /* don't bother scanning if we don't have a pattern matcher ctx
                         * which means we don't have uricontent sigs */
                        if (det_ctx->sgh->mpm_uri_ctx != NULL) {
                            if (det_ctx->sgh->mpm_uricontent_maxlen <= p->http_uri.raw_size[det_ctx->pkt_cnt]) {
                                if (det_ctx->sgh->mpm_uricontent_maxlen == 1)      det_ctx->pkts_uri_scanned1++;
                                else if (det_ctx->sgh->mpm_uricontent_maxlen == 2) det_ctx->pkts_uri_scanned2++;
                                else if (det_ctx->sgh->mpm_uricontent_maxlen == 3) det_ctx->pkts_uri_scanned3++;
                                else if (det_ctx->sgh->mpm_uricontent_maxlen == 4) det_ctx->pkts_uri_scanned4++;
                                else                                           det_ctx->pkts_uri_scanned++;

                                det_ctx->pmq.mode = PMQ_MODE_SCAN;
                                ret = mpm_table[det_ctx->sgh->mpm_uri_ctx->mpm_type].Scan(det_ctx->sgh->mpm_uri_ctx, &det_ctx->mtcu, &det_ctx->pmq, p->http_uri.raw[det_ctx->pkt_cnt], p->http_uri.raw_size[det_ctx->pkt_cnt]);
                                if (ret > 0) {
                                    if (det_ctx->sgh->mpm_uricontent_maxlen == 1)      det_ctx->pkts_uri_searched1++;
                                    else if (det_ctx->sgh->mpm_uricontent_maxlen == 2) det_ctx->pkts_uri_searched2++;
                                    else if (det_ctx->sgh->mpm_uricontent_maxlen == 3) det_ctx->pkts_uri_searched3++;
                                    else if (det_ctx->sgh->mpm_uricontent_maxlen == 4) det_ctx->pkts_uri_searched4++;
                                    else                                           det_ctx->pkts_uri_searched++;

                                    det_ctx->pmq.mode = PMQ_MODE_SEARCH;
                                    ret += mpm_table[det_ctx->sgh->mpm_uri_ctx->mpm_type].Search(det_ctx->sgh->mpm_uri_ctx, &det_ctx->mtcu, &det_ctx->pmq, p->http_uri.raw[det_ctx->pkt_cnt], p->http_uri.raw_size[det_ctx->pkt_cnt]);

                                    /* indicate to uricontent that we have a uri,
                                     * we scanned it _AND_ we found pattern matches. */
                                    det_ctx->de_have_httpuri = TRUE;
                                }
                            }
                        }
#endif
                    } else {
                        if (pe->flags & DETECT_PCRE_CAPTURE_PKT) {
                            PktVarAdd(p, pe->capname, (uint8_t *)str_ptr, ret);
                        } else if (pe->flags & DETECT_PCRE_CAPTURE_FLOW) {
                            FlowVarAddStr(p->flow, pe->capidx, (uint8_t *)str_ptr, ret);
                        }
                    }
                }
            }
            /* update ptrs for pcre RELATIVE */
            det_ctx->pkt_ptr =  ptr+ov[1];
            det_ctx->pkt_off = (ptr+ov[1]) - p->payload;
            //printf("DetectPcre: post match: t->pkt_ptr %p t->pkt_off %" PRIu32 "\n", t->pkt_ptr, t->pkt_off);

            ret = 1;
        }

    } else {
        SCLogDebug("pcre had matching error");
        ret = 0;
    }

    SCReturnInt(ret);
}

DetectPcreData *DetectPcreParse (char *regexstr)
{
    const char *eb;
    int eo;
    int opts = 0;
    DetectPcreData *pd = NULL;
    char *re = NULL, *op_ptr = NULL, *op = NULL;
#define MAX_SUBSTRINGS 30
    int ret = 0, res = 0;
    int ov[MAX_SUBSTRINGS];

    uint16_t slen = strlen(regexstr);
    uint16_t pos = 0;
    uint8_t negate = 0;

    while (pos < slen && isspace(regexstr[pos])) {
        pos++;
    }

    if (regexstr[pos] == '!') {
        negate = 1;
        pos++;
    }

    ret = pcre_exec(parse_regex, parse_regex_study, regexstr+pos, slen-pos, 0, 0, ov, MAX_SUBSTRINGS);
    if (ret < 0) {
        SCLogError(SC_ERR_PCRE_MATCH, "parse error");
        goto error;
    }

    if (ret > 1) {
        const char *str_ptr;
        res = pcre_get_substring((char *)regexstr+pos, ov, MAX_SUBSTRINGS, 1, &str_ptr);
        if (res < 0) {
            SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed");
            return NULL;
        }
        re = (char *)str_ptr;

        if (ret > 2) {
            res = pcre_get_substring((char *)regexstr+pos, ov, MAX_SUBSTRINGS, 2, &str_ptr);
            if (res < 0) {
                SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed");
                return NULL;
            }
            op_ptr = op = (char *)str_ptr;
        }
    }
    //printf("ret %" PRId32 " re \'%s\', op \'%s\'\n", ret, re, op);

    pd = SCMalloc(sizeof(DetectPcreData));
    if (pd == NULL) {
        SCLogError(SC_ERR_MEM_ALLOC, "malloc failed");
        goto error;
    }
    memset(pd, 0, sizeof(DetectPcreData));

    if (negate)
        pd->negate = 1;

    if (op != NULL) {
        while (*op) {
            SCLogDebug("regex option %c", *op);

            switch (*op) {
                case 'A':
                    opts |= PCRE_ANCHORED;
                    break;
                case 'E':
                    opts |= PCRE_DOLLAR_ENDONLY;
                    break;
                case 'G':
                    opts |= PCRE_UNGREEDY;
                    break;

                case 'i':
                    opts |= PCRE_CASELESS;
                    break;
                case 'm':
                    opts |= PCRE_MULTILINE;
                    break;
                case 's':
                    opts |= PCRE_DOTALL;
                    break;
                case 'x':
                    opts |= PCRE_EXTENDED;
                    break;

                case 'B': /* snort's option */
                    pd->flags |= DETECT_PCRE_RAWBYTES;
                    break;
                case 'R': /* snort's option */
                    pd->flags |= DETECT_PCRE_RELATIVE;
                    break;
                case 'U': /* snort's option */
                    pd->flags |= DETECT_PCRE_URI;
                    break;
                case 'O':
                    pd->flags |= DETECT_PCRE_MATCH_LIMIT;
                    break;
                case 'P':
                    /* snort's option (http body inspeciton, chunks loaded from HTP) */
                    pd->flags |= DETECT_PCRE_HTTP_BODY_AL;
                    break;
                default:
                    SCLogError(SC_ERR_UNKNOWN_REGEX_MOD, "unknown regex modifier '%c'", *op);
                    goto error;
            }
            op++;
        }
    }

    //printf("DetectPcreParse: \"%s\"\n", re);

    pd->re = pcre_compile(re, opts, &eb, &eo, NULL);
    if(pd->re == NULL)  {
        SCLogError(SC_ERR_PCRE_COMPILE, "pcre compile of \"%s\" failed at offset %" PRId32 ": %s", regexstr, eo, eb);
        goto error;
    }

    pd->sd = pcre_study(pd->re, 0, &eb);
    if(eb != NULL)  {
        SCLogError(SC_ERR_PCRE_STUDY, "pcre study failed : %s", eb);
        goto error;
    }

    if(pd->sd == NULL)
        pd->sd = (pcre_extra *) SCCalloc(1,sizeof(pcre_extra));

    if(pd->sd)  {

        if(pd->flags & DETECT_PCRE_MATCH_LIMIT) {

            if(pcre_match_limit >= -1)    {
                pd->sd->match_limit = pcre_match_limit;
                pd->sd->flags |= PCRE_EXTRA_MATCH_LIMIT;
            }
#ifndef NO_PCRE_MATCH_RLIMIT
            if(pcre_match_limit_recursion >= -1)    {
                pd->sd->match_limit_recursion = pcre_match_limit_recursion;
                pd->sd->flags |= PCRE_EXTRA_MATCH_LIMIT_RECURSION;
            }
#endif /* NO_PCRE_MATCH_RLIMIT */
        }
        else    {

            pd->sd->match_limit = MATCH_LIMIT_DEFAULT;
            pd->sd->flags |= PCRE_EXTRA_MATCH_LIMIT;
#ifndef NO_PCRE_MATCH_RLIMIT
            pd->sd->match_limit_recursion = MATCH_LIMIT_DEFAULT;
            pd->sd->flags |= PCRE_EXTRA_MATCH_LIMIT_RECURSION;
#endif /* NO_PCRE_MATCH_RLIMIT */
        }

    }

    if (re != NULL) SCFree(re);
    if (op_ptr != NULL) SCFree(op_ptr);
    return pd;

error:
    if (re != NULL) SCFree(re);
    if (op_ptr != NULL) SCFree(op_ptr);
    if (pd != NULL && pd->re != NULL) pcre_free(pd->re);
    if (pd != NULL && pd->sd != NULL) pcre_free(pd->sd);
    if (pd) SCFree(pd);
    return NULL;
}

DetectPcreData *DetectPcreParseCapture(char *regexstr, DetectEngineCtx *de_ctx, DetectPcreData *pd)
{
    int ret = 0, res = 0;
    int ov[MAX_SUBSTRINGS];
    const char *capture_str_ptr = NULL, *type_str_ptr = NULL;

    if(pd == NULL)
        goto error;

    if(de_ctx == NULL)
        goto error;
    //printf("DetectPcreParseCapture: \'%s\'\n", regexstr);

    ret = pcre_exec(parse_capture_regex, parse_capture_regex_study, regexstr, strlen(regexstr), 0, 0, ov, MAX_SUBSTRINGS);
    if (ret > 1) {
        res = pcre_get_substring((char *)regexstr, ov, MAX_SUBSTRINGS, 1, &type_str_ptr);
        if (res < 0) {
            SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed");
            goto error;
        }
        res = pcre_get_substring((char *)regexstr, ov, MAX_SUBSTRINGS, 2, &capture_str_ptr);
        if (res < 0) {
            SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed");
            goto error;
        }
    }
    //printf("DetectPcreParseCapture: type \'%s\'\n", type_str_ptr ? type_str_ptr : "NULL");
    //printf("DetectPcreParseCapture: capture \'%s\'\n", capture_str_ptr ? capture_str_ptr : "NULL");

    if (capture_str_ptr != NULL) {
        pd->capname = SCStrdup((char *)capture_str_ptr);
    }

    if (type_str_ptr != NULL) {
        if (strcmp(type_str_ptr,"pkt") == 0) {
            pd->flags |= DETECT_PCRE_CAPTURE_PKT;
        } else if (strcmp(type_str_ptr,"flow") == 0) {
            pd->flags |= DETECT_PCRE_CAPTURE_FLOW;
        }
        if (capture_str_ptr != NULL) {
            if (pd->flags & DETECT_PCRE_CAPTURE_PKT)
                pd->capidx = VariableNameGetIdx(de_ctx,(char *)capture_str_ptr,DETECT_PKTVAR);
            else if (pd->flags & DETECT_PCRE_CAPTURE_FLOW)
                pd->capidx = VariableNameGetIdx(de_ctx,(char *)capture_str_ptr,DETECT_FLOWVAR);
        }
    }
    //printf("DetectPcreParseCapture: pd->capname %s\n", pd->capname ? pd->capname : "NULL");

    if (type_str_ptr != NULL) pcre_free((char *)type_str_ptr);
    if (capture_str_ptr != NULL) pcre_free((char *)capture_str_ptr);
    return pd;

error:
    if (pd != NULL && pd->capname != NULL) SCFree(pd->capname);
    if (pd) SCFree(pd);
    return NULL;

}

int DetectPcreSetup (DetectEngineCtx *de_ctx, Signature *s, SigMatch *m, char *regexstr)
{
    DetectPcreData *pd = NULL;
    SigMatch *sm = NULL;

    pd = DetectPcreParse(regexstr);
    if (pd == NULL) goto error;

    pd = DetectPcreParseCapture(regexstr, de_ctx, pd);
    if (pd == NULL) goto error;

    sm = SigMatchAlloc();
    if (sm == NULL)
        goto error;

    sm->type = DETECT_PCRE;
    sm->ctx = (void *)pd;

    if (pd->flags & DETECT_PCRE_HTTP_BODY_AL) {
        sm->type = DETECT_PCRE_HTTPBODY;

        SCLogDebug("Body inspection modifier set");
        s->flags |= SIG_FLAG_APPLAYER;
        pcre_need_htp_request_body = 1;
    }

    SigMatchAppend(s,m,sm);

    return 0;

error:
    if (pd != NULL) DetectPcreFree(pd);
    if (sm != NULL) SCFree(sm);
    return -1;
}

void DetectPcreFree(void *ptr) {
    DetectPcreData *pd = (DetectPcreData *)ptr;

    if (pd->capname != NULL) SCFree(pd->capname);
    if (pd->re != NULL) pcre_free(pd->re);
    if (pd->sd != NULL) pcre_free(pd->sd);

    SCFree(pd);
    return;
}

#ifdef UNITTESTS /* UNITTESTS */

/**
 * \test DetectPcreParseTest01 make sure we don't allow invalid opts 7.
 */
static int DetectPcreParseTest01 (void) {
    int result = 1;
    DetectPcreData *pd = NULL;
    char *teststring = "/blah/7";

    pd = DetectPcreParse(teststring);
    if (pd != NULL) {
        printf("expected NULL: got %p", pd);
        result = 0;
        DetectPcreFree(pd);
    }
    return result;
}

/**
 * \test DetectPcreParseTest02 make sure we don't allow invalid opts Ui$.
 */
static int DetectPcreParseTest02 (void) {
    int result = 1;
    DetectPcreData *pd = NULL;
    char *teststring = "/blah/Ui$";

    pd = DetectPcreParse(teststring);
    if (pd != NULL) {
        printf("expected NULL: got %p", pd);
        result = 0;
        DetectPcreFree(pd);
    }
    return result;
}

/**
 * \test DetectPcreParseTest03 make sure we don't allow invalid opts UZi.
 */
static int DetectPcreParseTest03 (void) {
    int result = 1;
    DetectPcreData *pd = NULL;
    char *teststring = "/blah/UZi";

    pd = DetectPcreParse(teststring);
    if (pd != NULL) {
        printf("expected NULL: got %p", pd);
        result = 0;
        DetectPcreFree(pd);
    }
    return result;
}

/**
 * \test DetectPcreParseTest04 make sure we allow escaped "
 */
static int DetectPcreParseTest04 (void) {
    int result = 1;
    DetectPcreData *pd = NULL;
    char *teststring = "/b\\\"lah/i";

    pd = DetectPcreParse(teststring);
    if (pd == NULL) {
        printf("expected %p: got NULL", pd);
        result = 0;
    }

    DetectPcreFree(pd);
    return result;
}

/**
 * \test DetectPcreParseTest05 make sure we parse pcre with no opts
 */
static int DetectPcreParseTest05 (void) {
    int result = 1;
    DetectPcreData *pd = NULL;
    char *teststring = "/b(l|a)h/";

    pd = DetectPcreParse(teststring);
    if (pd == NULL) {
        printf("expected %p: got NULL", pd);
        result = 0;
    }

    DetectPcreFree(pd);
    return result;
}

/**
 * \test DetectPcreParseTest06 make sure we parse pcre with smi opts
 */
static int DetectPcreParseTest06 (void) {
    int result = 1;
    DetectPcreData *pd = NULL;
    char *teststring = "/b(l|a)h/smi";

    pd = DetectPcreParse(teststring);
    if (pd == NULL) {
        printf("expected %p: got NULL", pd);
        result = 0;
    }

    DetectPcreFree(pd);
    return result;
}

/**
 * \test DetectPcreParseTest07 make sure we parse pcre with /Ui opts
 */
static int DetectPcreParseTest07 (void) {
    int result = 1;
    DetectPcreData *pd = NULL;
    char *teststring = "/blah/Ui";

    pd = DetectPcreParse(teststring);
    if (pd == NULL) {
        printf("expected %p: got NULL", pd);
        result = 0;
    }

    DetectPcreFree(pd);
    return result;
}

/**
 * \test DetectPcreParseTest08 make sure we parse pcre with O opts
 */
static int DetectPcreParseTest08 (void) {
    int result = 1;
    DetectPcreData *pd = NULL;
    char *teststring = "/b(l|a)h/O";

    pd = DetectPcreParse(teststring);
    if (pd == NULL) {
        printf("expected %p: got NULL", pd);
        result = 0;
    }

    DetectPcreFree(pd);
    return result;
}

static int DetectPcreTestSig01Real(int mpm_type) {
    uint8_t *buf = (uint8_t *)
        "GET /one/ HTTP/1.1\r\n"
        "Host: one.example.org\r\n"
        "\r\n\r\n"
        "GET /two/ HTTP/1.1\r\n"
        "Host: two.example.org\r\n"
        "\r\n\r\n";
    uint16_t buflen = strlen((char *)buf);
    Packet p;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;
    int result = 0;
    Flow f;

    memset(&f, 0, sizeof(f));
    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    p.src.family = AF_INET;
    p.dst.family = AF_INET;
    p.payload = buf;
    p.payload_len = buflen;
    p.proto = IPPROTO_TCP;
    p.flow = &f;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->mpm_matcher = mpm_type;
    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx,"alert tcp any any -> any any (msg:\"HTTP TEST\"; pcre:\"^/gEt/i\"; pcre:\"/\\/two\\//U; pcre:\"/GET \\/two\\//\"; pcre:\"/\\s+HTTP/R\"; sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);
    if (PacketAlertCheck(&p, 1) == 1) {
        result = 1;
    }

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    DetectEngineCtxFree(de_ctx);
end:
    return result;
}

static int DetectPcreTestSig02Real(int mpm_type) {
    uint8_t *buf = (uint8_t *)
        "GET /one/ HTTP/1.1\r\n"
        "Host: one.example.org\r\n"
        "\r\n\r\n"
        "GET /two/ HTTP/1.1\r\n"
        "Host: two.example.org\r\n"
        "\r\n\r\n";
    uint16_t buflen = strlen((char *)buf);
    Packet p;
    Flow f;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;
    int result = 0;

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    memset(&f, 0, sizeof(f));
    p.flow = &f;
    p.src.family = AF_INET;
    p.dst.family = AF_INET;
    p.payload = buf;
    p.payload_len = buflen;
    p.proto = IPPROTO_TCP;

    pcre_match_limit = 100;
    pcre_match_limit_recursion = 100;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->mpm_matcher = mpm_type;
    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx,"alert tcp any any -> any any (msg:\"HTTP TEST\"; pcre:\"/two/O\"; sid:2;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);
    if (PacketAlertCheck(&p, 2) == 1) {
        result = 1;
    }

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    DetectEngineCtxFree(de_ctx);
end:
    return result;
}

static int DetectPcreTestSig01B2g (void) {
    return DetectPcreTestSig01Real(MPM_B2G);
}
static int DetectPcreTestSig01B3g (void) {
    return DetectPcreTestSig01Real(MPM_B3G);
}
static int DetectPcreTestSig01Wm (void) {
    return DetectPcreTestSig01Real(MPM_WUMANBER);
}

static int DetectPcreTestSig02B2g (void) {
    return DetectPcreTestSig02Real(MPM_B2G);
}
static int DetectPcreTestSig02B3g (void) {
    return DetectPcreTestSig02Real(MPM_B3G);
}
static int DetectPcreTestSig02Wm (void) {
    return DetectPcreTestSig02Real(MPM_WUMANBER);
}

/**
 * \test DetectPcreTestSig03Real negation test ! outside of "" this sig should not match
 */
static int DetectPcreTestSig03Real(int mpm_type) {
    uint8_t *buf = (uint8_t *)
        "GET /one/ HTTP/1.1\r\n"
        "Host: one.example.org\r\n"
        "\r\n\r\n"
        "GET /two/ HTTP/1.1\r\n"
        "Host: two.example.org\r\n"
        "\r\n\r\n";
    uint16_t buflen = strlen((char *)buf);
    Packet p;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;
    int result = 1;

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    p.src.family = AF_INET;
    p.dst.family = AF_INET;
    p.payload = buf;
    p.payload_len = buflen;
    p.proto = IPPROTO_TCP;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        result = 0;
        goto end;
    }

    de_ctx->mpm_matcher = mpm_type;
    de_ctx->flags |= DE_QUIET;

    de_ctx->sig_list = SigInit(de_ctx,"alert tcp any any -> any any (msg:\"HTTP TEST\"; content:\"GET\"; pcre:!\"/two/\"; sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);
    if (PacketAlertCheck(&p, 1)){
        printf("sid 1 matched even though it shouldn't have:");
        result = 0;
    }
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    DetectEngineCtxFree(de_ctx);
end:
    return result;
}

static int DetectPcreTestSig03B2g (void) {
    return DetectPcreTestSig03Real(MPM_B2G);
}
static int DetectPcreTestSig03B3g (void) {
    return DetectPcreTestSig03Real(MPM_B3G);
}
static int DetectPcreTestSig03Wm (void) {
    return DetectPcreTestSig03Real(MPM_WUMANBER);
}

/**
 * \test Check the signature with pcre modifier P (match with L7 to http body data)
 */
static int DetectPcreModifPTest04(void) {
    int result = 0;
    uint8_t httpbuf1[] =
        "GET / HTTP/1.1\r\n"
        "Host: www.emergingthreats.net\r\n"
        "User-Agent: Mozilla/5.0 (X11; U; Linux i686; es-ES; rv:1.9.0.13) Gecko/2009080315 Ubuntu/8.10 (intrepid) Firefox/3.0.13\r\n"
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9;q=0.8\r\n"
        "Accept-Language: es-es,es;q=0.8,en-us;q=0.5,en;q=0.3\r\n"
        "Accept-Encoding: gzip,deflate\r\n"
        "Accept-Charset: ISO-8859-1,utf-8;q=0.7,*;q=0.7\r\n"
        "Date: Tue, 22 Sep 2009 19:24:48 GMT\r\n"
        "Server: Apache\r\n"
        "X-Powered-By: PHP/5.2.5\r\n"
        "P3P: CP=\"NOI ADM DEV PSAi COM NAV OUR OTRo STP IND DEM\"\r\n"
        "Expires: Mon, 1 Jan 2001 00:00:00 GMT\r\n"
        "Last-Modified: Tue, 22 Sep 2009 19:24:48 GMT\r\n"
        "Cache-Control: no-store, no-cache, must-revalidate, post-check=0, pre-check=0\r\n"
        "Pragma: no-cache\r\n"
        "Keep-Alive: timeout=15, max=100\r\n"
        "Connection: Keep-Alive\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "\r\n"
        "88b7\r\n"
        "\r\n"
        "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\" \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\r\n"
        "\r\n"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"en-gb\" lang=\"en-gb\">\r\n\r\n";

    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    Packet p;
    Flow f;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p.src.family = AF_INET;
    p.dst.family = AF_INET;
    p.payload = NULL;
    p.payload_len = 0;
    p.proto = IPPROTO_TCP;

    f.protoctx = (void *)&ssn;
    p.flow = &f;
    p.flowflags |= FLOW_PKT_TOSERVER;
    ssn.alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);
    StreamL7DataPtrInit(&ssn);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert http any any -> any any (msg:"
                                   "\"Pcre modifier P\"; pcre:\"/DOCTYPE/P\"; "
                                   "sid:1;)");
    if (s == NULL) {
        goto end;
    }

    s->next = SigInit(de_ctx,"alert http any any -> any any (msg:\""
                          "Pcre modifier P (no match)\"; pcre:\"/blah/P\"; sid:2;)");
    if (s->next == NULL) {
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    int r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER, httpbuf1, httplen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    HtpState *http_state = ssn.aldata[AlpGetStateIdx(ALPROTO_HTTP)];
    if (http_state == NULL) {
        printf("no http state: ");
        result = 0;
        goto end;
    }

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);

    if (!(PacketAlertCheck(&p, 1))) {
        printf("sid 1 didn't match but should have: ");
        goto end;
    }
    if (PacketAlertCheck(&p, 2)) {
        printf("sid 2 matched but shouldn't: ");
        goto end;
    }

    result = 1;
end:
    if (de_ctx != NULL) SigGroupCleanup(de_ctx);
    if (de_ctx != NULL) SigCleanSignatures(de_ctx);
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);

    StreamL7DataPtrFree(&ssn);
    StreamTcpFreeConfig(TRUE);
    return result;
}

/**
 * \test Check the signature with pcre modifier P (match with L7 to http body data)
 *       over fragmented chunks (DOCTYPE fragmented)
 */
static int DetectPcreModifPTest05(void) {
    int result = 0;
    uint8_t httpbuf1[] =
        "GET / HTTP/1.1\r\n"
        "Host: www.emergingthreats.net\r\n"
        "User-Agent: Mozilla/5.0 (X11; U; Linux i686; es-ES; rv:1.9.0.13) Gecko/2009080315 Ubuntu/8.10 (intrepid) Firefox/3.0.13\r\n"
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9;q=0.8\r\n"
        "Accept-Language: es-es,es;q=0.8,en-us;q=0.5,en;q=0.3\r\n"
        "Accept-Encoding: gzip,deflate\r\n"
        "Accept-Charset: ISO-8859-1,utf-8;q=0.7,*;q=0.7\r\n"
        "Date: Tue, 22 Sep 2009 19:24:48 GMT\r\n"
        "Server: Apache\r\n"
        "X-Powered-By: PHP/5.2.5\r\n"
        "P3P: CP=\"NOI ADM DEV PSAi COM NAV OUR OTRo STP IND DEM\"\r\n"
        "Expires: Mon, 1 Jan 2001 00:00:00 GMT\r\n"
        "Last-Modified: Tue, 22 Sep 2009 19:24:48 GMT\r\n"
        "Cache-Control: no-store, no-cache, must-revalidate, post-check=0, pre-check=0\r\n"
        "Pragma: no-cache\r\n"
        "Keep-Alive: timeout=15, max=100\r\n"
        "Connection: Keep-Alive\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "\r\n"
        "88b7\r\n"
        "\r\n"
        "<!DOC";

    uint8_t httpbuf2[] = "TYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\" \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\r\n"
        "\r\n"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\" xml:lang=\"en-gb\" lang=\"en-gb\">\r\n\r\n";

    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */
    TcpSession ssn;
    Packet p1;
    Packet p2;
    Flow f;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;

    memset(&th_v, 0, sizeof(th_v));
    memset(&p1, 0, sizeof(Packet));
    memset(&p2, 0, sizeof(Packet));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p1.src.family = AF_INET;
    p1.dst.family = AF_INET;
    p1.payload = NULL;
    p1.payload_len = 0;
    p1.proto = IPPROTO_TCP;

    p2.src.family = AF_INET;
    p2.dst.family = AF_INET;
    p2.payload = NULL;
    p2.payload_len = 0;
    p2.proto = IPPROTO_TCP;

    f.protoctx = (void *)&ssn;
    p1.flow = &f;
    p1.flowflags |= FLOW_PKT_TOSERVER;
    p2.flow = &f;
    p2.flowflags |= FLOW_PKT_TOSERVER;
    ssn.alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);
    StreamL7DataPtrInit(&ssn);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert http any any -> any any (msg:"
                                   "\"Pcre modifier P\"; pcre:\"/DOC/P\"; "
                                   "sid:1;)");
    if (s == NULL) {
        goto end;
    }

    s->next = SigInit(de_ctx,"alert http any any -> any any (msg:\""
                          "Pcre modifier P (no match)\"; pcre:\"/DOCTYPE/P\"; sid:2;)");
    if (s->next == NULL) {
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    int r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER, httpbuf1, httplen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    r = AppLayerParse(&f, ALPROTO_HTTP, STREAM_TOSERVER, httpbuf2, httplen2);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    HtpState *http_state = ssn.aldata[AlpGetStateIdx(ALPROTO_HTTP)];
    if (http_state == NULL) {
        printf("no http state: ");
        result = 0;
        goto end;
    }

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p1);
    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p2);

    if ( !(PacketAlertCheck(&p1, 1))) {
        printf("sid 1 didn't match but should have");
        goto end;
    }

    if ( !(PacketAlertCheck(&p1, 2))) {
        printf("sid 2 didn't match but should have");
        /* It's a partial match over 2 chunks*/
        goto end;
    }

    if ( !(PacketAlertCheck(&p2, 2))) {
        printf("sid 2 didn't match but should have");
        /* It's a partial match over 2 chunks*/
        goto end;
    }

    result = 1;
end:
    if (de_ctx != NULL) SigGroupCleanup(de_ctx);
    if (de_ctx != NULL) SigCleanSignatures(de_ctx);
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);

    StreamL7DataPtrFree(&ssn);
    StreamTcpFreeConfig(TRUE);
    return result;
}

#endif /* UNITTESTS */

/**
 * \brief this function registers unit tests for DetectPcre
 */
void DetectPcreRegisterTests(void) {
#ifdef UNITTESTS /* UNITTESTS */
    UtRegisterTest("DetectPcreParseTest01", DetectPcreParseTest01, 1);
    UtRegisterTest("DetectPcreParseTest02", DetectPcreParseTest02, 1);
    UtRegisterTest("DetectPcreParseTest03", DetectPcreParseTest03, 1);
    UtRegisterTest("DetectPcreParseTest04", DetectPcreParseTest04, 1);
    UtRegisterTest("DetectPcreParseTest05", DetectPcreParseTest05, 1);
    UtRegisterTest("DetectPcreParseTest06", DetectPcreParseTest06, 1);
    UtRegisterTest("DetectPcreParseTest07", DetectPcreParseTest07, 1);
    UtRegisterTest("DetectPcreParseTest08", DetectPcreParseTest08, 1);
    UtRegisterTest("DetectPcreTestSig01B2g -- pcre test", DetectPcreTestSig01B2g, 1);
    UtRegisterTest("DetectPcreTestSig01B3g -- pcre test", DetectPcreTestSig01B3g, 1);
    UtRegisterTest("DetectPcreTestSig01Wm -- pcre test", DetectPcreTestSig01Wm, 1);
    UtRegisterTest("DetectPcreTestSig02B2g -- pcre test", DetectPcreTestSig02B2g, 1);
    UtRegisterTest("DetectPcreTestSig02B3g -- pcre test", DetectPcreTestSig02B3g, 1);
    UtRegisterTest("DetectPcreTestSig02Wm -- pcre test", DetectPcreTestSig02Wm, 1);
    UtRegisterTest("DetectPcreTestSig03B2g -- negated pcre test", DetectPcreTestSig03B2g, 1);
    UtRegisterTest("DetectPcreTestSig03B3g -- negated pcre test", DetectPcreTestSig03B3g, 1);
    UtRegisterTest("DetectPcreTestSig03Wm -- negated pcre test", DetectPcreTestSig03Wm, 1);
    UtRegisterTest("DetectPcreModifPTest04 -- Modifier P", DetectPcreModifPTest04, 1);
    UtRegisterTest("DetectPcreModifPTest05 -- Modifier P fragmented", DetectPcreModifPTest05, 1);
#endif /* UNITTESTS */
}

