// Small services: parental control, nifm, and other mostly-stubbed interfaces.
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

static void ok_logger(Service *s, IpcReq *req, IpcResp *resp) {
    LOG("[svc] %s cmd %u (ok)\n", s->name, req->cmd);
    resp_size(resp, 0x10);
}

static uint32_t new_event_handle(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

// ------------------------------------------------------------------ pctl
static void pctl_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1031: case 1035: case 1018: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // restrictions off
    case 1032: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;
    default: LOG("[pctl] IParentalControlService cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

static void pctl_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0 || req->cmd == 1)
        ipc_return_object(resp, service_new("IParentalControlService", pctl_service, NULL));
}
SERVICE("pctl", pctl) { pctl_root(s, req, resp); }
SERVICE("pctl:s", pctl_s) { pctl_root(s, req, resp); }
SERVICE("pctl:a", pctl_a) { pctl_root(s, req, resp); }
SERVICE("pctl:r", pctl_r) { pctl_root(s, req, resp); }

// ------------------------------------------------------------------ nifm (network interface): always offline
// FE3H asks for the console's IP address when the calendar first opens (its online features); with the logging stub
// CreateGeneralService returned no object and the SDK's GetCurrentPrimaryIpAddress used a null service.
#define NIFM_NO_INTERNET MAKE_RESULT(110, 300)
#define NIFM_NOT_ACCEPTED MAKE_RESULT(110, 112)

static void nifm_request(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: OUT(uint32_t, 0) = 1; resp_size(resp, 4); break;  // GetRequestState: Error (no network)
    case 1: resp->result = NIFM_NOT_ACCEPTED; break;            // GetResult
    case 2:                                                     // GetSystemEventReadableHandles
        resp->copy_handles[resp->ncopy++] = new_event_handle();
        resp->copy_handles[resp->ncopy++] = new_event_handle();
        break;
    default: break;  // Cancel, Submit, SetRequirement..., SetConnectionConfirmationOption...: ok
    }
}

static void nifm_general(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1: {  // GetClientId(out buffer)
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size >= 4) *(uint32_t *)(uintptr_t)b->addr = 1;
        break;
    }
    case 4: ipc_return_object(resp, service_new("nifm:IRequest", nifm_request, NULL)); break;  // CreateRequest
    case 12: case 15: case 18: resp->result = NIFM_NO_INTERNET; break;  // current IP address / IP config / status
    case 21: case 22: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // IsAny{Internet,Foreground}RequestAccepted
    default: LOG("[nifm] IGeneralService cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

static void nifm_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 4 || req->cmd == 5)  // CreateGeneralServiceOld / CreateGeneralService
        ipc_return_object(resp, service_new("nifm:IGeneralService", nifm_general, NULL));
    else
        LOG("[nifm] static service cmd %u (ok)\n", req->cmd);
}
SERVICE("nifm:u", nifm_u) { nifm_root(s, req, resp); }
SERVICE("nifm:a", nifm_a) { nifm_root(s, req, resp); }
SERVICE("nifm:s", nifm_s) { nifm_root(s, req, resp); }
