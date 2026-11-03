/* PlayFab-backed GRTS implementation. Separate from the legacy WGS store. */
HRESULT pfx_config_create( struct PFXGameSaveConfigRequest *request, PFXGameSaveConfigHandle *config );
void pfx_config_close( PFXGameSaveConfigHandle config );
HRESULT pfx_folder_async( PFXGameSaveConfigHandle config, XAsyncBlock *async );
HRESULT pfx_folder_result( XAsyncBlock *async, SIZE_T size, char *folder );
HRESULT pfx_remaining_quota( PFXGameSaveConfigHandle config, INT64 *quota );
HRESULT pfx_set_ui( struct PFXGameSaveUiCallbacks *callbacks );
HRESULT pfx_progress( XUserHandle user, UINT32 *state, UINT64 *current, UINT64 *total );
HRESULT pfx_ui_response( XUserHandle user, UINT32 kind, UINT32 response );
HRESULT pfx_upload_async( PFXGameSaveConfigHandle config, UINT32 options, XAsyncBlock *async );
HRESULT pfx_status_result( XAsyncBlock *async );
HRESULT pfx_description_async( PFXGameSaveConfigHandle config, const char *description, XAsyncBlock *async );
HRESULT pfx_set_device_callback( void *callback, void *context );
