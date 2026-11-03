/*
 * PlayFab-backed Gaming Runtime file saves.
 *
 * The PFX ABI is separate from legacy XGameSave/WGS. Use the title's native
 * PlayFab SDK for authenticated synchronization, manifests, conflicts and
 * quotas. A private SDK instance avoids recursively selecting this GRTS
 * facade. No successful cloud result is synthesized here.
 *
 * LGPL-2.1-or-later; same license as the surrounding Wine runtime.
 */
#include "private.h"
#include "pfxgamesave.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(pfxgamesave);

struct pf_init_args { XTaskQueueHandle queue; UINT64 options; const char *folder; };
struct pf_descriptor
{
    INT64 time;
    UINT64 total, uploaded;
    char device_type[256], device_id[256], friendly_name[256], thumbnail[2048], description[4096];
};
struct pf_ui_callbacks
{
    void (WINAPI *progress)(void *, UINT32, void *); void *progress_context;
    void (WINAPI *failed)(void *, UINT32, HRESULT, void *); void *failed_context;
    void (WINAPI *contention)(void *, struct pf_descriptor *, struct pf_descriptor *, void *); void *contention_context;
    void (WINAPI *conflict)(void *, struct pf_descriptor *, struct pf_descriptor *, void *); void *conflict_context;
    void (WINAPI *storage)(void *, BOOLEAN, UINT64, void *); void *storage_context;
};
static struct
{
    HMODULE core, save;
    HRESULT (WINAPI *create_service)(const char *, const char *, void **);
    void (WINAPI *close_service)(void *);
    HRESULT (WINAPI *create_user)(void *, XUserHandle, void *, void **);
    void (WINAPI *close_user)(void *);
    INT32 (WINAPI *compare_user)(void *, void *);
    HRESULT (WINAPI *initialize)(struct pf_init_args *);
    HRESULT (WINAPI *add_user)(void *, UINT32, XAsyncBlock *);
    HRESULT (WINAPI *add_result)(XAsyncBlock *);
    HRESULT (WINAPI *folder_size)(void *, SIZE_T *);
    HRESULT (WINAPI *folder)(void *, SIZE_T, char *, SIZE_T *);
    HRESULT (WINAPI *quota)(void *, INT64 *);
    HRESULT (WINAPI *connected)(void *, BOOLEAN *);
    HRESULT (WINAPI *upload)(void *, UINT32, XAsyncBlock *);
    HRESULT (WINAPI *upload_result)(XAsyncBlock *);
    HRESULT (WINAPI *description)(void *, const char *, XAsyncBlock *);
    HRESULT (WINAPI *description_result)(XAsyncBlock *);
    HRESULT (WINAPI *set_ui)(struct pf_ui_callbacks *);
    HRESULT (WINAPI *progress)(void *, UINT32 *, UINT64 *, UINT64 *);
    HRESULT (WINAPI *response[5])(void *, UINT32);
    HRESULT (WINAPI *set_device)(XTaskQueueHandle, void *, void *);
} api;

struct PFXGameSaveConfig
{
    struct list entry;
    LONG ref;
    XUserHandle user;
    void *service, *local_user;
    UINT32 flags;
    BOOL ready_online;
};
struct pfx_work
{
    XAsyncBlock inner;
    XAsyncBlock *outer;
    struct PFXGameSaveConfig *config;
    UINT32 kind;
};
struct folder_result { char *folder; HRESULT status; };
static struct list configs = LIST_INIT(configs);
static struct PFXGameSaveUiCallbacks callbacks;
static PFXGameSaveDeviceChangedCallback *device_callback;
static void *device_context;
static WCHAR backend_root[4096];
static CRITICAL_SECTION pfx_cs;
static CRITICAL_SECTION_DEBUG pfx_cs_debug =
{
    0, 0, &pfx_cs, { &pfx_cs_debug.ProcessLocksList, &pfx_cs_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": pfx_cs") }
};
static CRITICAL_SECTION pfx_cs = { &pfx_cs_debug, -1, 0, 0, 0, 0 };
static INIT_ONCE backend_once = INIT_ONCE_STATIC_INIT;
static HRESULT backend_status;

static struct PFXGameSaveConfig *acquire_config( PFXGameSaveConfigHandle handle )
{
    struct PFXGameSaveConfig *c, *found = NULL;
    EnterCriticalSection(&pfx_cs);
    LIST_FOR_EACH_ENTRY(c, &configs, struct PFXGameSaveConfig, entry)
        if (c == handle) { InterlockedIncrement(&c->ref); found = c; break; }
    LeaveCriticalSection(&pfx_cs);
    return found;
}

struct final_upload
{
    XAsyncBlock async;
    HANDLE event;
    LONG ref;
    HRESULT result;
};

static void release_final_upload(struct final_upload *work)
{
    if (InterlockedDecrement(&work->ref)) return;
    CloseHandle(work->event);
    free(work);
}

static void CALLBACK final_upload_complete(XAsyncBlock *async)
{
    struct final_upload *work = async->context;
    work->result = api.upload_result(async);
    SetEvent(work->event);
    release_final_upload(work);
}

/* GRTS owns a final synchronization after the title releases its config.
 * This in-process bridge must finish it before the title tears down Core.
 * Use a runtime-dispatched queue, never the title's possibly Manual queue. */
static BOOL flush_before_close(struct PFXGameSaveConfig *config)
{
    struct final_upload *work;
    HRESULT hr;
    DWORD waited;
    if (!config->ready_online) return TRUE;
    if (!(work = calloc(1, sizeof(*work))))
    { WARN("final save synchronization could not allocate its operation.\n"); return TRUE; }
    work->event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!work->event) { free(work); return TRUE; }
    work->ref = 2;
    work->async.context = work; work->async.callback = final_upload_complete;
    hr = api.upload(config->local_user, 1 /* ReleaseDeviceAsActive */, &work->async);
    if (FAILED(hr))
    {
        WARN("final save upload did not start, hr %#lx; local saves retained.\n", hr);
        release_final_upload(work); release_final_upload(work);
        return TRUE;
    }
    waited = WaitForSingleObject(work->event, 30000);
    if (waited != WAIT_OBJECT_0)
    {
        /* The SDK may still reference the user. Retain its config for process
         * lifetime on timeout instead of freeing a pending operation's handles.
         * No cloud success is reported; local game files remain untouched. */
        WARN("final save upload did not finish before shutdown; local saves retained.\n");
        release_final_upload(work);
        return FALSE;
    }
    hr = work->result;
    if (FAILED(hr)) WARN("final save upload failed, hr %#lx; local saves retained.\n", hr);
    else TRACE("final save upload completed before config close.\n");
    release_final_upload(work);
    return TRUE;
}

static void release_config( struct PFXGameSaveConfig *c )
{
    if (InterlockedDecrement(&c->ref)) return;
    if (!flush_before_close(c)) return;
    if (c->local_user) api.close_user(c->local_user);
    if (c->service) api.close_service(c->service);
    if (c->user) IXUserImpl_XUserCloseHandle(x_user_impl, c->user);
    free(c);
}

/* Do not call foreign SDK code while holding our registry lock. */
static struct PFXGameSaveConfig *config_for_local_user( void *user )
{
    struct PFXGameSaveConfig *c, *items[32], *found = NULL;
    SIZE_T count = 0, i;
    EnterCriticalSection(&pfx_cs);
    LIST_FOR_EACH_ENTRY(c, &configs, struct PFXGameSaveConfig, entry)
        if (count < ARRAY_SIZE(items)) { InterlockedIncrement(&c->ref); items[count++] = c; }
    LeaveCriticalSection(&pfx_cs);
    for (i = 0; i < count; i++)
    {
        if (!found && !api.compare_user(user, items[i]->local_user)) found = items[i];
        else release_config(items[i]);
    }
    return found;
}

static struct PFXGameSaveConfig *config_for_xuser( XUserHandle user )
{
    struct PFXGameSaveConfig *c, *found = NULL;
    EnterCriticalSection(&pfx_cs);
    LIST_FOR_EACH_ENTRY(c, &configs, struct PFXGameSaveConfig, entry)
        if (c->user == user) { InterlockedIncrement(&c->ref); found = c; break; }
    LeaveCriticalSection(&pfx_cs);
    return found;
}

static BOOL make_dirs( WCHAR *path )
{
    WCHAR *p;
    for (p = path + 3; *p; p++) if (*p == '\\')
    {
        *p = 0;
        if (!CreateDirectoryW(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) { *p = '\\'; return FALSE; }
        *p = '\\';
    }
    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static char *to_utf8( const WCHAR *wide )
{
    SIZE_T size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    char *s = malloc(size);
    if (s) WideCharToMultiByte(CP_UTF8, 0, wide, -1, s, size, NULL, NULL);
    return s;
}

static HRESULT initialize_private_sdk( const char *root )
{
    static const WCHAR key_name[] = L"Software\\Microsoft\\GamingServices";
    static const WCHAR name[] = L"ForceUseInprocGameSaves";
    BYTE saved[256]; DWORD type = 0, size = sizeof(saved), one = 1;
    LONG status, prior;
    HKEY key;
    HRESULT hr;
    struct pf_init_args args = { NULL, 0, root };

    /* The primary GDK SDK has already selected its GRTS provider before it
     * calls PFXInitializeConfig. Only the private instance selects the native
     * in-process sync engine here. Restore the prefix's exact setting even
     * when initialization fails; never leave an override for older titles. */
    status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key_name, 0, NULL, 0,
                            KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &key, NULL);
    if (status) return HRESULT_FROM_WIN32(status);
    prior = RegQueryValueExW(key, name, NULL, &type, saved, &size);
    if (prior != ERROR_SUCCESS && prior != ERROR_FILE_NOT_FOUND)
    { RegCloseKey(key); return HRESULT_FROM_WIN32(prior); }
    status = RegSetValueExW(key, name, 0, REG_DWORD, (BYTE *)&one, sizeof(one));
    hr = status ? HRESULT_FROM_WIN32(status) : api.initialize(&args);
    if (prior == ERROR_SUCCESS) status = RegSetValueExW(key, name, 0, type, saved, size);
    else status = RegDeleteValueW(key, name);
    RegCloseKey(key);
    if (status && status != ERROR_FILE_NOT_FOUND) return HRESULT_FROM_WIN32(status);
    return hr;
}

static void WINAPI ui_progress(void *, UINT32, void *);
static void WINAPI ui_failed(void *, UINT32, HRESULT, void *);
static void WINAPI ui_contention(void *, struct pf_descriptor *, struct pf_descriptor *, void *);
static void WINAPI ui_conflict(void *, struct pf_descriptor *, struct pf_descriptor *, void *);
static void WINAPI ui_storage(void *, BOOLEAN, UINT64, void *);
static void WINAPI device_changed(void *, struct pf_descriptor *, void *);

static BOOL CALLBACK load_backend( INIT_ONCE *once, void *parameter, void **context )
{
    WCHAR source[4096], destination[4096], path[4096];
    char *family = xgr_package_family(), *root = NULL;
    HMODULE original = GetModuleHandleW(L"PlayFabGameSave.dll");
    FILETIME modified;
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    struct pf_ui_callbacks ui = { ui_progress, NULL, ui_failed, NULL, ui_contention, NULL,
                                  ui_conflict, NULL, ui_storage, NULL };
    backend_status = HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    api.core = GetModuleHandleW(L"PlayFabCore.dll");
    if (!original || !api.core || !family) goto done;
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", path, ARRAY_SIZE(path))) goto done;
    if (wcslen(path) + strlen(family) + 100 >= ARRAY_SIZE(path))
    { backend_status = E_INVALIDARG; goto done; }
    swprintf(backend_root, ARRAY_SIZE(backend_root), L"%s\\Packages\\%S\\SystemAppData\\PFXGameSave", path, family);
    if (!make_dirs(backend_root)) { backend_status = HRESULT_FROM_WIN32(GetLastError()); goto done; }
    if (!GetModuleFileNameW(original, source, ARRAY_SIZE(source)) ||
        !GetFileAttributesExW(source, GetFileExInfoStandard, &attributes)) goto done;
    modified = attributes.ftLastWriteTime;
    swprintf(path, ARRAY_SIZE(path), L"%s\\backend", backend_root);
    if (!make_dirs(path)) { backend_status = HRESULT_FROM_WIN32(GetLastError()); goto done; }
    swprintf(destination, ARRAY_SIZE(destination), L"%s\\pgs-%08lx-%08lx-%08lx.dll", path,
             modified.dwHighDateTime, modified.dwLowDateTime, attributes.nFileSizeLow);
    if (!CopyFileW(source, destination, TRUE) && GetLastError() != ERROR_FILE_EXISTS)
    { backend_status = HRESULT_FROM_WIN32(GetLastError()); goto done; }
    api.save = LoadLibraryExW(destination, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!api.save) { backend_status = HRESULT_FROM_WIN32(GetLastError()); goto done; }
#define LOAD(field, module, name) do { if (!(api.field = (void *)GetProcAddress(api.module, name))) \
    { backend_status = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND); goto done; } } while (0)
    LOAD(create_service, core, "PFServiceConfigCreateHandle");
    LOAD(close_service, core, "PFServiceConfigCloseHandle");
    LOAD(create_user, core, "PFLocalUserCreateHandleWithXboxUser");
    LOAD(close_user, core, "PFLocalUserCloseHandle");
    LOAD(compare_user, core, "PFLocalUserHandleCompare");
    LOAD(initialize, save, "PFGameSaveFilesInitialize");
    LOAD(add_user, save, "PFGameSaveFilesAddUserWithUiAsync");
    LOAD(add_result, save, "PFGameSaveFilesAddUserWithUiResult");
    LOAD(folder_size, save, "PFGameSaveFilesGetFolderSize");
    LOAD(folder, save, "PFGameSaveFilesGetFolder");
    LOAD(quota, save, "PFGameSaveFilesGetRemainingQuota");
    LOAD(connected, save, "PFGameSaveFilesIsConnectedToCloud");
    LOAD(upload, save, "PFGameSaveFilesUploadWithUiAsync");
    LOAD(upload_result, save, "PFGameSaveFilesUploadWithUiResult");
    LOAD(description, save, "PFGameSaveFilesSetSaveDescriptionAsync");
    LOAD(description_result, save, "PFGameSaveFilesSetSaveDescriptionResult");
    LOAD(set_ui, save, "PFGameSaveFilesSetUiCallbacks");
    LOAD(progress, save, "PFGameSaveFilesUiProgressGetProgress");
    LOAD(response[0], save, "PFGameSaveFilesSetUiProgressResponse");
    LOAD(response[1], save, "PFGameSaveFilesSetUiSyncFailedResponse");
    LOAD(response[2], save, "PFGameSaveFilesSetUiActiveDeviceContentionResponse");
    LOAD(response[3], save, "PFGameSaveFilesSetUiConflictResponse");
    LOAD(response[4], save, "PFGameSaveFilesSetUiOutOfStorageResponse");
    LOAD(set_device, save, "PFGameSaveFilesSetActiveDeviceChangedCallback");
#undef LOAD
    swprintf(path, ARRAY_SIZE(path), L"%s\\data", backend_root);
    if (!make_dirs(path)) { backend_status = HRESULT_FROM_WIN32(GetLastError()); goto done; }
    if (!(root = to_utf8(path))) { backend_status = E_OUTOFMEMORY; goto done; }
    backend_status = initialize_private_sdk(root);
    if (SUCCEEDED(backend_status)) backend_status = api.set_ui(&ui);
    if (SUCCEEDED(backend_status)) backend_status = api.set_device(NULL, device_changed, NULL);
done:
    free(family); free(root);
    if (FAILED(backend_status)) WARN("PlayFab backend initialization failed, hr %#lx.\n", backend_status);
    else TRACE("authenticated native PlayFab save backend initialized.\n");
    /* One process owns the loaded SDK instance. No library unload may race a
     * callback; process teardown reclaims it after outstanding game operations. */
    return TRUE;
}

HRESULT pfx_config_create( struct PFXGameSaveConfigRequest *request, PFXGameSaveConfigHandle *out )
{
    struct PFXGameSaveConfig *c;
    HRESULT hr;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!request || !request->requestingUser || !request->titleId || !*request->titleId ||
        !request->apiUrl || strncmp(request->apiUrl, "https://", 8) || (request->flags & ~15u)) return E_INVALIDARG;
    /* Entity-only clients require a distinct identity binding; don't silently
     * authenticate them as the default Xbox account. */
    if (request->flags & 4) return E_NOTIMPL;
    InitOnceExecuteOnce(&backend_once, load_backend, NULL, NULL);
    if (FAILED(backend_status)) return backend_status;
    if (request->flags & 8) return E_NOTIMPL; /* custom roots need their own SDK instance */
    if (!(c = calloc(1, sizeof(*c)))) return E_OUTOFMEMORY;
    c->ref = 1; c->flags = request->flags;
    hr = IXUserImpl_XUserDuplicateHandle(x_user_impl, request->requestingUser, &c->user);
    if (SUCCEEDED(hr)) hr = api.create_service(request->apiUrl, request->titleId, &c->service);
    if (SUCCEEDED(hr)) hr = api.create_user(c->service, c->user, NULL, &c->local_user);
    if (FAILED(hr)) { release_config(c); return hr; }
    EnterCriticalSection(&pfx_cs); list_add_tail(&configs, &c->entry); LeaveCriticalSection(&pfx_cs);
    *out = c;
    TRACE("created a PlayFab save configuration for title %s.\n", debugstr_a(request->titleId));
    return S_OK;
}

void pfx_config_close( PFXGameSaveConfigHandle handle )
{
    struct PFXGameSaveConfig *c, *found = NULL;
    EnterCriticalSection(&pfx_cs);
    LIST_FOR_EACH_ENTRY(c, &configs, struct PFXGameSaveConfig, entry)
        if (c == handle) { list_remove(&c->entry); found = c; break; }
    LeaveCriticalSection(&pfx_cs);
    if (found) release_config(found);
}

static void CALLBACK work_complete( XAsyncBlock *inner )
{
    struct pfx_work *w = inner->context;
    struct PFXGameSaveConfig *c = w->config;
    struct folder_result *result = NULL;
    HRESULT hr;
    SIZE_T size = 0;
    BOOLEAN connected = FALSE;
    if (w->kind == 0)
    {
        hr = api.add_result(inner);
        if (SUCCEEDED(hr)) hr = api.folder_size(c->local_user, &size);
        if (SUCCEEDED(hr) && (!size || size > 1024 * 1024)) hr = E_FAIL;
        if (SUCCEEDED(hr))
        {
            if (!(result = calloc(1, sizeof(*result))) || !(result->folder = malloc(size))) hr = E_OUTOFMEMORY;
            else hr = api.folder(c->local_user, size, result->folder, NULL);
        }
        if (SUCCEEDED(hr)) hr = api.connected(c->local_user, &connected);
        c->ready_online = SUCCEEDED(hr) && connected;
        if (SUCCEEDED(hr) && !connected) hr = (HRESULT)0x80830004; /* real offline outcome */
        if (result) result->status = hr;
        if (FAILED(hr) && hr != (HRESULT)0x80830004 && result)
        { free(result->folder); free(result); result = NULL; }
        xgr_async_complete_ex(w->outer, hr, result, result ? size : 0);
    }
    else
    {
        hr = w->kind == 1 ? api.upload_result(inner) : api.description_result(inner);
        xgr_async_complete_ex(w->outer, hr, NULL, 0);
    }
    TRACE("PlayFab operation %u completed, hr %#lx.\n", w->kind, hr);
    release_config(c); free(w);
}

static HRESULT start_work( PFXGameSaveConfigHandle handle, XAsyncBlock *outer, UINT32 kind,
                           UINT32 options, const char *description )
{
    struct PFXGameSaveConfig *c;
    struct pfx_work *w;
    HRESULT hr;
    if (!outer || (kind == 2 && !description)) return E_INVALIDARG;
    if (!(c = acquire_config(handle))) return E_INVALIDARG;
    if (!(w = calloc(1, sizeof(*w)))) { release_config(c); return E_OUTOFMEMORY; }
    w->config = c; w->outer = outer; w->kind = kind;
    w->inner.queue = outer->queue; w->inner.context = w; w->inner.callback = work_complete;
    if (!kind) hr = api.add_user(c->local_user, c->flags & 3, &w->inner);
    else if (kind == 1) hr = api.upload(c->local_user, options, &w->inner);
    else hr = api.description(c->local_user, description, &w->inner);
    if (FAILED(hr)) { release_config(c); free(w); }
    return hr;
}

HRESULT pfx_folder_async( PFXGameSaveConfigHandle c, XAsyncBlock *a ) { return start_work(c, a, 0, 0, NULL); }
HRESULT pfx_upload_async( PFXGameSaveConfigHandle c, UINT32 o, XAsyncBlock *a )
{ return o > 1 ? E_INVALIDARG : start_work(c, a, 1, o, NULL); }
HRESULT pfx_description_async( PFXGameSaveConfigHandle c, const char *s, XAsyncBlock *a )
{ return !s || strlen(s) > 768 ? E_INVALIDARG : start_work(c, a, 2, 0, s); }

HRESULT pfx_folder_result( XAsyncBlock *async, SIZE_T size, char *folder )
{
    struct folder_result *r;
    HRESULT hr;
    SIZE_T needed;
    if (!async || !folder) return E_INVALIDARG;
    if (!xgr_async_completed(async, &hr, (void **)&r)) return E_PENDING;
    if (!r) return FAILED(hr) ? hr : E_FAIL;
    needed = strlen(r->folder) + 1;
    if (size < needed) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    memcpy(folder, r->folder, needed); hr = r->status;
    free(r->folder); free(r); async->internal[2] = NULL;
    return hr;
}

HRESULT pfx_status_result( XAsyncBlock *async )
{
    HRESULT hr;
    if (!async) return E_INVALIDARG;
    return xgr_async_completed(async, &hr, NULL) ? hr : E_PENDING;
}

HRESULT pfx_remaining_quota( PFXGameSaveConfigHandle handle, INT64 *quota )
{
    struct PFXGameSaveConfig *c;
    HRESULT hr;
    if (!quota || !(c = acquire_config(handle))) return E_INVALIDARG;
    hr = api.quota(c->local_user, quota); release_config(c); return hr;
}

HRESULT pfx_set_ui( struct PFXGameSaveUiCallbacks *ui )
{
    if (!ui) return E_INVALIDARG;
    EnterCriticalSection(&pfx_cs); callbacks = *ui; LeaveCriticalSection(&pfx_cs); return S_OK;
}

HRESULT pfx_set_device_callback( void *callback, void *context )
{
    EnterCriticalSection(&pfx_cs); device_callback = callback; device_context = context;
    LeaveCriticalSection(&pfx_cs); return S_OK;
}

HRESULT pfx_progress( XUserHandle user, UINT32 *state, UINT64 *current, UINT64 *total )
{
    struct PFXGameSaveConfig *c = config_for_xuser(user);
    HRESULT hr;
    if (!c) return E_INVALIDARG;
    hr = api.progress(c->local_user, state, current, total); release_config(c); return hr;
}

HRESULT pfx_ui_response( XUserHandle user, UINT32 kind, UINT32 response )
{
    static const UINT32 max_response[] = { 0, 2, 2, 2, 1 };
    struct PFXGameSaveConfig *c;
    HRESULT hr;
    if (kind >= ARRAY_SIZE(max_response) || response > max_response[kind] ||
        !(c = config_for_xuser(user))) return E_INVALIDARG;
    hr = api.response[kind](c->local_user, response); release_config(c); return hr;
}

static struct PFXGameSaveUiCallbacks snapshot_ui(void)
{
    struct PFXGameSaveUiCallbacks ui;
    EnterCriticalSection(&pfx_cs); ui = callbacks; LeaveCriticalSection(&pfx_cs); return ui;
}

static void convert_descriptor( struct pf_descriptor *p, struct PFXGameSaveDescriptor *out )
{
    memset(out, 0, sizeof(*out));
    if (!p) return;
    out->deviceType = p->device_type; out->deviceId = p->device_id; out->friendlyName = p->friendly_name;
    out->thumbnailUri = p->thumbnail; out->shortSaveDescription = p->description;
    out->time = p->time; out->totalBytes = p->total; out->uploadedBytes = p->uploaded;
}

static void WINAPI ui_progress( void *user, UINT32 state, void *context )
{
    struct PFXGameSaveConfig *c = config_for_local_user(user);
    struct PFXGameSaveUiCallbacks ui = snapshot_ui();
    if (c) { if (ui.progressCallback) ui.progressCallback(c->user, state, ui.progressContext); release_config(c); }
}

static void WINAPI ui_failed( void *user, UINT32 state, HRESULT error, void *context )
{
    struct PFXGameSaveConfig *c = config_for_local_user(user);
    struct PFXGameSaveUiCallbacks ui = snapshot_ui();
    int response;
    if (!c) { api.response[1](user, 0); return; }
    if (ui.syncFailedCallback) ui.syncFailedCallback(c->user, state, error, ui.syncFailedContext);
    else
    {
        response = MessageBoxW(NULL, L"Cloud save synchronization failed. Retry?\nYes: retry. No: play with local saves offline. Cancel: stop.",
                               L"Game Saves", MB_YESNOCANCEL | MB_ICONWARNING);
        api.response[1](user, response == IDYES ? 1 : response == IDNO ? 2 : 0);
    }
    release_config(c);
}

static void descriptor_ui( void *user, struct pf_descriptor *local, struct pf_descriptor *remote, BOOL conflict )
{
    struct PFXGameSaveConfig *c = config_for_local_user(user);
    struct PFXGameSaveUiCallbacks ui = snapshot_ui();
    struct PFXGameSaveDescriptor l, r;
    int response;
    if (!c) { api.response[conflict ? 3 : 2](user, 0); return; }
    convert_descriptor(local, &l); convert_descriptor(remote, &r);
    if (conflict && ui.conflictWithDescriptorCallback) ui.conflictWithDescriptorCallback(c->user, &l, &r, ui.conflictWithDescriptorContext);
    else if (!conflict && ui.activeDeviceContentionWithDescriptorCallback) ui.activeDeviceContentionWithDescriptorCallback(c->user, &l, &r, ui.activeDeviceContentionWithDescriptorContext);
    else if (conflict && ui.conflictCallback) ui.conflictCallback(c->user, l.time, r.time, l.totalBytes, r.totalBytes, ui.conflictContext);
    else if (!conflict && ui.activeDeviceContentionCallback) ui.activeDeviceContentionCallback(c->user, l.time, r.time, ui.activeDeviceContentionContext);
    else if (conflict)
    {
        response = MessageBoxW(NULL, L"Cloud and local saves conflict. Load the cloud save?\nYes: cloud. No: local. Cancel: stop.", L"Game Saves", MB_YESNOCANCEL | MB_ICONWARNING);
        api.response[3](user, response == IDYES ? 2 : response == IDNO ? 1 : 0);
    }
    else
    {
        response = MessageBoxW(NULL, L"Another device is active. Use its latest cloud save?", L"Game Saves", MB_OKCANCEL | MB_ICONWARNING);
        api.response[2](user, response == IDOK ? 2 : 0);
    }
    release_config(c);
}
static void WINAPI ui_contention( void *u, struct pf_descriptor *l, struct pf_descriptor *r, void *c ) { descriptor_ui(u,l,r,FALSE); }
static void WINAPI ui_conflict( void *u, struct pf_descriptor *l, struct pf_descriptor *r, void *c ) { descriptor_ui(u,l,r,TRUE); }

static void WINAPI ui_storage( void *user, BOOLEAN partition, UINT64 bytes, void *context )
{
    struct PFXGameSaveConfig *c = config_for_local_user(user);
    struct PFXGameSaveUiCallbacks ui = snapshot_ui();
    int response;
    if (!c) { api.response[4](user, 0); return; }
    if (ui.outOfStorageCallback) ui.outOfStorageCallback(c->user, partition, bytes, ui.outOfStorageContext);
    else
    {
        response = MessageBoxW(NULL, L"There is not enough storage for game saves. Free space and retry, or cancel.", L"Game Saves", MB_RETRYCANCEL | MB_ICONWARNING);
        api.response[4](user, response == IDRETRY ? 1 : 0);
    }
    release_config(c);
}

static void WINAPI device_changed( void *user, struct pf_descriptor *device, void *context )
{
    struct PFXGameSaveConfig *c = config_for_local_user(user);
    struct PFXGameSaveDescriptor descriptor;
    PFXGameSaveDeviceChangedCallback *callback;
    void *callback_context;
    if (!c) return;
    convert_descriptor(device, &descriptor);
    EnterCriticalSection(&pfx_cs); callback = device_callback; callback_context = device_context; LeaveCriticalSection(&pfx_cs);
    if (callback) callback(c->user, &descriptor, callback_context);
    release_config(c);
}
