/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef PVZ2_WINDOWS_IDENTITY_H
#define PVZ2_WINDOWS_IDENTITY_H

#include <windows.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propsys.h>
#include <string.h>

/* Every taskbar-capable host window explicitly keeps the process identity, including windows made before the
 * game. Reading the process value also lets the standalone Settings test use this helper without host globals. */
static inline HRESULT pvz2_window_app_id(HWND hwnd)
{
    PWSTR id = NULL;
    HRESULT hr = GetCurrentProcessExplicitAppUserModelID(&id);
    if (FAILED(hr)) return hr;
    IPropertyStore *store = NULL;
    hr = SHGetPropertyStoreForWindow(hwnd, &IID_IPropertyStore, (void **)&store);
    if (SUCCEEDED(hr)) {
        PROPVARIANT pv;
        memset(&pv, 0, sizeof(pv));
        pv.vt = VT_LPWSTR;
        pv.pwszVal = id;
        hr = store->lpVtbl->SetValue(store, &PKEY_AppUserModel_ID, &pv);
        store->lpVtbl->Release(store);
    }
    CoTaskMemFree(id);
    return hr;
}

#endif
