/*
 * Copyright 2005 Jacek Caban
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include <stdarg.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "winreg.h"
#include "objbase.h"
#include "oaidl.h"
#include "shlwapi.h"

#define ATL_INITGUID
#include "atliface.h"

#include "wine/debug.h"
#include "wine/unicode.h"

WINE_DEFAULT_DEBUG_CHANNEL(atl);

/**************************************************************
 * ATLRegistrar implementation
 */

static struct {
    WCHAR name[22];
    HKEY  key;
} root_keys[] = {
    {{'H','K','E','Y','_','C','L','A','S','S','E','S','_','R','O','O','T',0},
                    HKEY_CLASSES_ROOT},
    {{'H','K','E','Y','_','C','U','R','R','E','N','T','_','U','S','E','R',0},
                    HKEY_CURRENT_USER},
    {{'H','K','E','Y','_','L','O','C','A','L','_','M','A','C','H','I','N','E',0},
                    HKEY_LOCAL_MACHINE},
    {{'H','K','E','Y','_','U','S','E','R','S',0},
                    HKEY_USERS},
    {{'H','K','E','Y','_','P','E','R','F','O','R','M','A','N','C','E','_','D','A','T','A',0},
                    HKEY_PERFORMANCE_DATA},
    {{'H','K','E','Y','_','D','Y','N','_','D','A','T','A',0},
                    HKEY_DYN_DATA},
    {{'H','K','E','Y','_','C','U','R','R','E','N','T','_','C','O','N','F','I','G',0},
                    HKEY_CURRENT_CONFIG},
    {{'H','K','C','R',0}, HKEY_CLASSES_ROOT},
    {{'H','K','C','U',0}, HKEY_CURRENT_USER},
    {{'H','K','L','M',0}, HKEY_LOCAL_MACHINE},
    {{'H','K','U',0},     HKEY_USERS},
    {{'H','K','P','D',0}, HKEY_PERFORMANCE_DATA},
    {{'H','K','D','D',0}, HKEY_DYN_DATA},
    {{'H','K','C','C',0}, HKEY_CURRENT_CONFIG}
};

typedef struct rep_list_str {
    LPOLESTR key;
    LPOLESTR item;
    int key_len;
    struct rep_list_str *next;
} rep_list;

typedef struct {
    IRegistrarVtbl *lpVtbl;
    ULONG ref;
    rep_list *rep;
} Registrar;

typedef struct {
    LPOLESTR str;
    DWORD alloc;
    DWORD len;
} strbuf;

static void strbuf_init(strbuf *buf)
{
    buf->str = HeapAlloc(GetProcessHeap(), 0, 128*sizeof(WCHAR));
    buf->alloc = 128;
    buf->len = 0;
}

static void strbuf_write(LPCOLESTR str, strbuf *buf, int len)
{
    if(len == -1)
        len = lstrlenW(str);
    if(buf->len+len+1 >= buf->alloc) {
        buf->alloc = (buf->len+len)<<1;
        buf->str = HeapReAlloc(GetProcessHeap(), 0, buf->str, buf->alloc*sizeof(WCHAR));
    }
    memcpy(buf->str+buf->len, str, len*sizeof(OLECHAR));
    buf->len += len;
    buf->str[buf->len] = '\0';
}

static HRESULT get_word(LPCOLESTR *str, strbuf *buf)
{
    LPCOLESTR iter, iter2 = *str;

    buf->len = 0;
    buf->str[0] = '\0';

    while(isspaceW(*iter2))
        iter2++;
    iter = iter2;
    if(!*iter) {
        *str = iter;
        return S_OK;
    }

    if(*iter == '{' || *iter == '}' || *iter == '=') {
        strbuf_write(iter++, buf, 1);
    }else if(*iter == '\'') {
        iter2 = ++iter;
        iter = strchrW(iter, '\'');
        if(!iter) {
            WARN("Unexpected end of script\n");
            *str = iter;
            return DISP_E_EXCEPTION;
        }
        strbuf_write(iter2, buf, iter-iter2);
        iter++;
    }else {
        while(*iter && !isspaceW(*iter))
            iter++;
        strbuf_write(iter2, buf, iter-iter2);
    }

    while(isspaceW(*iter))
        iter++;
    *str = iter;
    return S_OK;
}

static HRESULT do_preprocess(Registrar *This, LPCOLESTR data, strbuf *buf)
{
    LPCOLESTR iter, iter2 = data;
    rep_list *rep_iter;
    static const WCHAR wstr[] = {'%',0};

    iter = strchrW(data, '%');
    while(iter) {
        strbuf_write(iter2, buf, iter-iter2);

        iter2 = ++iter;
        if(!*iter2)
            return DISP_E_EXCEPTION;
        iter = strchrW(iter2, '%');
        if(!iter)
            return DISP_E_EXCEPTION;

        if(iter == iter2) {
            strbuf_write(wstr, buf, 1);
        }else {
            for(rep_iter = This->rep; rep_iter; rep_iter = rep_iter->next) {
                if(rep_iter->key_len == iter-iter2 
                        && !memcmp(iter2, rep_iter->key, rep_iter->key_len*sizeof(OLECHAR)))
                    break;
            }
            if(!rep_iter)
                return DISP_E_EXCEPTION;

            strbuf_write(rep_iter->item, buf, -1);
        }

        iter2 = ++iter;
        iter = strchrW(iter, '%');
    }

    strbuf_write(iter2, buf, -1);
    TRACE("%s\n", debugstr_w(buf->str));

    return S_OK;
}

static HRESULT do_process_key(LPCOLESTR *pstr, HKEY parent_key, strbuf *buf, BOOL do_register)
{
    LPCOLESTR iter = *pstr;
    BOOL no_remove = FALSE, is_val = FALSE, force_remove = FALSE, do_delete = FALSE;
    HRESULT hres;
    LONG lres;
    HKEY hkey = 0;
    strbuf name;

    static const WCHAR wstrNoRemove[] = {'N','o','R','e','m','o','v','e',0};
    static const WCHAR wstrForceRemove[] = {'F','o','r','c','e','R','e','m','o','v','e',0};
    static const WCHAR wstrDelete[] = {'D','e','l','e','t','e',0};
    static const WCHAR wstrval[] = {'v','a','l',0};

    iter = *pstr;
    hres = get_word(&iter, buf);
    if(FAILED(hres))
        return hres;
    strbuf_init(&name);

    while(buf->str[1] || buf->str[0] != '}') {
        if(!lstrcmpW(buf->str, wstrNoRemove)) {
            no_remove = TRUE;
        }else if(!lstrcmpW(buf->str, wstrForceRemove)) {
            force_remove = TRUE;
        }else if(!lstrcmpW(buf->str, wstrval)) {
            is_val = TRUE;
        }else if(!lstrcmpW(buf->str, wstrDelete)) {
            do_delete = TRUE;
        }else {
            TRACE("name = %s\n", debugstr_w(buf->str));
            if(do_register) {
                if(is_val) {
                    if(force_remove || no_remove || do_delete) {
                        WARN("Attribites for value!\n");
                        hres = DISP_E_EXCEPTION;
                        break;
                    }
                    hkey = parent_key;
                    strbuf_write(buf->str, &name, -1);
                }else if(do_delete) {
                    TRACE("Deleting %s\n", debugstr_w(buf->str));
                    lres = SHDeleteKeyW(parent_key, buf->str);
                }else {
                    if(force_remove)
                        SHDeleteKeyW(parent_key, buf->str);
                    lres = RegCreateKeyW(parent_key, buf->str, &hkey);
                    if(lres != ERROR_SUCCESS) {
                        WARN("Could not create(open) key: %08lx\n", lres);
                        hres = HRESULT_FROM_WIN32(lres);
                        break;
                    }
                }
            }else if(!is_val && !do_delete) {
                strbuf_write(buf->str, &name, -1);
                lres = RegOpenKeyW(parent_key, buf->str, &hkey);
                if(lres != ERROR_SUCCESS)
                    WARN("Could not open key %s: %08lx\n", debugstr_w(name.str), lres);
            }
            if(!do_delete && *iter == '=') {
                iter++;
                hres = get_word(&iter, buf);
                if(FAILED(hres))
                    break;
                if(buf->len != 1) {
                    WARN("Wrong registry type: %s\n", debugstr_w(buf->str));
                    hres = DISP_E_EXCEPTION;
                    break;
                }
                if(do_register) {
                    switch(buf->str[0]) {
                    case 's':
                        hres = get_word(&iter, buf);
                        if(FAILED(hres))
                            break;
                        lres = RegSetValueExW(hkey, name.len ? name.str :  NULL, 0, REG_SZ, (PBYTE)buf->str,
                                (lstrlenW(buf->str)+1)*sizeof(WCHAR));
                        if(lres != ERROR_SUCCESS) {
                            WARN("Could set value of key: %08lx\n", lres);
                            hres = HRESULT_FROM_WIN32(lres);
                            break;
                        }
                        break;
                    case 'd': {
                        DWORD dw;
                        if(*iter == '0' && iter[1] == 'x') {
                            iter += 2;
                            dw = strtolW(iter, (WCHAR**)&iter, 16);
                        }else {
                            dw = strtolW(iter, (WCHAR**)&iter, 10);
                        }
                        lres = RegSetValueExW(hkey, name.len ? name.str :  NULL, 0, REG_DWORD,
                                (PBYTE)&dw, sizeof(dw));
                        if(lres != ERROR_SUCCESS) {
                            WARN("Could set value of key: %08lx\n", lres);
                            hres = HRESULT_FROM_WIN32(lres);
                            break;
                        }
                        break;
                    }
                    default:
                        WARN("Wrong resource type: %s\n", debugstr_w(buf->str));
                        hres = DISP_E_EXCEPTION;
                    };
                    if(FAILED(hres))
                        break;
                }else {
                    if(*iter == '-')
                        iter++;
                    hres = get_word(&iter, buf);
                    if(FAILED(hres))
                        break;
                }
            }else if(is_val) {
                WARN("value not set!\n");
                hres = DISP_E_EXCEPTION;
                break;
            }
            if(!is_val && !do_delete && *iter == '{') {
                hres = get_word(&iter, buf);
                if(FAILED(hres))
                    break;
                hres = do_process_key(&iter, hkey, buf, do_register);
                if(FAILED(hres))
                    break;
            }
            if(!do_register && !do_delete && !is_val && !no_remove) {
                TRACE("Deleting %s\n", debugstr_w(name.str));
                RegDeleteKeyW(parent_key, name.str);
            }
            if(hkey && !is_val)
                RegCloseKey(hkey);
            hkey = 0;
            name.len = 0;
            no_remove = is_val = force_remove = do_delete = FALSE;
        }
        hres = get_word(&iter, buf);
        if(FAILED(hres))
            break;
    }

    HeapFree(GetProcessHeap(), 0, name.str);
    if(hkey && !is_val)
        RegCloseKey(hkey);
    *pstr = iter;
    return hres;
}

static HRESULT do_process_root_key(LPCOLESTR data, BOOL do_register)
{
    LPCOLESTR iter = data;
    strbuf buf;
    HRESULT hres = S_OK;
    int i;

    strbuf_init(&buf);
    hres = get_word(&iter, &buf);
    if(FAILED(hres))
        return hres;

    while(*iter) {
        if(!buf.len) {
            WARN("ward.len == 0, failed\n");
            hres = DISP_E_EXCEPTION;
            break;
        }
        for(i=0; i<sizeof(root_keys)/sizeof(root_keys[0]); i++) {
            if(!lstrcmpW(buf.str, root_keys[i].name))
                break;
        }
        if(i == sizeof(root_keys)/sizeof(root_keys[0])) {
            WARN("Wrong root key name: %s\n", debugstr_w(buf.str));
            hres = DISP_E_EXCEPTION;
            break;
        }
        hres = get_word(&iter, &buf);
        if(FAILED(hres))
            break;
        if(buf.str[1] || buf.str[0] != '{') {
            WARN("Failed, expected '{', got %s\n", debugstr_w(buf.str));
            hres = DISP_E_EXCEPTION;
            break;
        }
        hres = do_process_key(&iter, root_keys[i].key, &buf, do_register);
        if(FAILED(hres)) {
            WARN("Processing key failed: %08lx\n", hres);
            break;
        }
        hres = get_word(&iter, &buf);
        if(FAILED(hres))
            break;
    }
    HeapFree(GetProcessHeap(), 0, buf.str);
    return hres;
}

static HRESULT string_register(Registrar *This, LPCOLESTR data, BOOL do_register)
{
    strbuf buf;
    HRESULT hres;

    TRACE("(%p %s %x)\n", This, debugstr_w(data), do_register);

    strbuf_init(&buf);
    hres = do_preprocess(This, data, &buf);
    if(FAILED(hres)) {
        WARN("preprocessing failed!\n");
        HeapFree(GetProcessHeap(), 0, buf.str);
        return hres;
    }

    hres = do_process_root_key(buf.str, do_register);
    if(FAILED(hres) && do_register)
        do_process_root_key(buf.str, FALSE);

    HeapFree(GetProcessHeap(), 0, buf.str);
    return hres;
}

static HRESULT resource_register(Registrar *This, LPCOLESTR resFileName,
                        UINT nID, LPCOLESTR szType, BOOL do_register)
{
    HINSTANCE hins;
    HRSRC src;
    LPSTR regstra;
    LPWSTR regstrw;
    DWORD len, reslen;
    HRESULT hres;

    hins = LoadLibraryExW(resFileName, NULL, LOAD_LIBRARY_AS_DATAFILE);
    if(hins) {
        src = FindResourceW(hins, (LPWSTR)nID, szType);
        if(src) {
            regstra = (LPSTR)LoadResource(hins, src);
            reslen = SizeofResource(hins, src);
            if(regstra) {
                len = MultiByteToWideChar(CP_ACP, 0, regstra, reslen, NULL, 0)+1;
                regstrw = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, len*sizeof(WCHAR));
                MultiByteToWideChar(CP_ACP, 0, regstra, reslen, regstrw, -1);
                regstrw[len-1] = '\0';

                hres = string_register(This, regstrw, do_register);

                HeapFree(GetProcessHeap(), 0, regstrw);
                HeapFree(GetProcessHeap(), 0, regstra);
            }else {
                WARN("could not load resource\n");
                hres = HRESULT_FROM_WIN32(GetLastError());
            }
        }else {
            WARN("Could not find source\n");
            hres = HRESULT_FROM_WIN32(GetLastError());
        }
        FreeLibrary(hins);
    }else {
        WARN("Could not load resource file\n");
        hres = HRESULT_FROM_WIN32(GetLastError());
    }

    return hres;
}

static HRESULT WINAPI Registrar_QueryInterface(IRegistrar *iface, REFIID riid, void **ppvObject)
{
    TRACE("(%p)->(%s %p\n", iface, debugstr_guid(riid), ppvObject);

    if(IsEqualGUID(&IID_IUnknown, riid) || IsEqualGUID(&IID_IRegistrar, riid)) {
        IRegistrar_AddRef(iface);
        *ppvObject = iface;
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG WINAPI Registrar_AddRef(IRegistrar *iface)
{
    Registrar *This = (Registrar*)iface;
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) ->%ld\n", This, ref);
    return ref;
}

static ULONG WINAPI Registrar_Release(IRegistrar *iface)
{
    Registrar *This = (Registrar*)iface;
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ->%ld\n", This, ref);
    if(!ref) {
        IRegistrar_ClearReplacements(iface);
        HeapFree(GetProcessHeap(), 0, This);
    }
    return ref;
}

static HRESULT WINAPI Registrar_AddReplacement(IRegistrar *iface, LPCOLESTR Key, LPCOLESTR item)
{
    Registrar *This = (Registrar*)iface;
    int len;
    rep_list *new_rep;

    TRACE("(%p)->(%s %s)\n", This, debugstr_w(Key), debugstr_w(item));

    new_rep = HeapAlloc(GetProcessHeap(), 0, sizeof(rep_list));

    new_rep->key_len  = lstrlenW(Key);
    new_rep->key = HeapAlloc(GetProcessHeap(), 0, new_rep->key_len*sizeof(OLECHAR)+1);
    memcpy(new_rep->key, Key, (new_rep->key_len+1)*sizeof(OLECHAR));

    len = lstrlenW(item)+1;
    new_rep->item = HeapAlloc(GetProcessHeap(), 0, len*sizeof(OLECHAR));
    memcpy(new_rep->item, item, len*sizeof(OLECHAR));

    new_rep->next = This->rep;
    This->rep = new_rep;
    
    return S_OK;
}

static HRESULT WINAPI Registrar_ClearReplacements(IRegistrar *iface)
{
    Registrar *This = (Registrar*)iface;
    rep_list *iter, *iter2;

    TRACE("(%p)\n", This);

    if(!This->rep)
        return S_OK;

    iter = This->rep;
    while(iter) {
        iter2 = iter->next;
        HeapFree(GetProcessHeap(), 0, iter->key);
        HeapFree(GetProcessHeap(), 0, iter->item);
        HeapFree(GetProcessHeap(), 0, iter);
        iter = iter2;
    }

    This->rep = NULL;
    return S_OK;
}

static HRESULT WINAPI Registrar_ResourceRegisterSz(IRegistrar* iface, LPCOLESTR resFileName,
                LPCOLESTR szID, LPCOLESTR szType)
{
    Registrar *This = (Registrar*)iface;
    FIXME("(%p)->(%s %s %s)\n", This, debugstr_w(resFileName), debugstr_w(szID), debugstr_w(szType));
    return E_NOTIMPL;
}

static HRESULT WINAPI Registrar_ResourceUnregisterSz(IRegistrar* iface, LPCOLESTR resFileName,
                LPCOLESTR szID, LPCOLESTR szType)
{
    Registrar *This = (Registrar*)iface;
    FIXME("(%p)->(%s %s %s)\n", This, debugstr_w(resFileName), debugstr_w(szID), debugstr_w(szType));
    return E_NOTIMPL;
}

static HRESULT WINAPI Registrar_FileRegister(IRegistrar* iface, LPCOLESTR fileName)
{
    Registrar *This = (Registrar*)iface;
    FIXME("(%p)->(%s)\n", This, debugstr_w(fileName));
    return E_NOTIMPL;
}

static HRESULT WINAPI Registrar_FileUnregister(IRegistrar* iface, LPCOLESTR fileName)
{
    Registrar *This = (Registrar*)iface;
    FIXME("(%p)->(%s)\n", This, debugstr_w(fileName));
    return E_NOTIMPL;
}

static HRESULT WINAPI Registrar_StringRegister(IRegistrar* iface, LPCOLESTR data)
{
    Registrar *This = (Registrar*)iface;
    TRACE("(%p)->(%s)\n", This, debugstr_w(data));
    return string_register(This, data, TRUE);
}

static HRESULT WINAPI Registrar_StringUnregister(IRegistrar* iface, LPCOLESTR data)
{
    Registrar *This = (Registrar*)iface;
    TRACE("(%p)->(%s)\n", This, debugstr_w(data));
    return string_register(This, data, FALSE);
}

static HRESULT WINAPI Registrar_ResourceRegister(IRegistrar* iface, LPCOLESTR resFileName,
                UINT nID, LPCOLESTR szType)
{
    Registrar *This = (Registrar*)iface;
    TRACE("(%p)->(%s %d %s)\n", iface, debugstr_w(resFileName), nID, debugstr_w(szType));
    return resource_register(This, resFileName, nID, szType, TRUE);
}

static HRESULT WINAPI Registrar_ResourceUnregister(IRegistrar* iface, LPCOLESTR resFileName,
                UINT nID, LPCOLESTR szType)
{
    Registrar *This = (Registrar*)iface;
    TRACE("(%p)->(%s %d %s)\n", This, debugstr_w(resFileName), nID, debugstr_w(szType));
    return resource_register(This, resFileName, nID, szType, FALSE);
}

static IRegistrarVtbl RegistrarVtbl = {
    Registrar_QueryInterface,
    Registrar_AddRef,
    Registrar_Release,
    Registrar_AddReplacement,
    Registrar_ClearReplacements,
    Registrar_ResourceRegisterSz,
    Registrar_ResourceUnregisterSz,
    Registrar_FileRegister,
    Registrar_FileUnregister,
    Registrar_StringRegister,
    Registrar_StringUnregister,
    Registrar_ResourceRegister,
    Registrar_ResourceUnregister,
};


/**************************************************************
 * ClassFactory implementation
 */

static HRESULT WINAPI RegistrarCF_QueryInterface(IClassFactory *iface, REFIID riid, void **ppvObject)
{
    TRACE("(%p)->(%s %p)\n", iface, debugstr_guid(riid), ppvObject);

    if(IsEqualGUID(&IID_IUnknown, riid) || IsEqualGUID(&IID_IRegistrar, riid)) {
        *ppvObject = iface;
        return S_OK;
    }

    return E_NOINTERFACE;
}

static ULONG WINAPI RegistrarCF_AddRef(IClassFactory *iface)
{
    return 2;
}

static ULONG WINAPI RegistrarCF_Release(IClassFactory *iface)
{
    return 1;
}

static HRESULT WINAPI RegistrarCF_CreateInstance(IClassFactory *iface, LPUNKNOWN pUnkOuter, REFIID riid, void **ppvObject)
{
    Registrar *ret;
    TRACE("(%p)->(%s %p)\n", iface, debugstr_guid(riid), ppvObject);

    if(!IsEqualGUID(&IID_IUnknown, riid) && !IsEqualGUID(&IID_IRegistrar, riid))
        return E_NOINTERFACE;

    ret = HeapAlloc(GetProcessHeap(), 0, sizeof(Registrar));
    ret->lpVtbl = &RegistrarVtbl;
    ret->ref = 1;
    ret->rep = NULL;
    *ppvObject = ret;

    return S_OK;
}

static HRESULT WINAPI RegistrarCF_LockServer(IClassFactory *iface, BOOL lock)
{
    FIXME("(%p)->(%x)\n", iface, lock);
    return S_OK;
}

static const IClassFactoryVtbl IRegistrarCFVtbl = {
    RegistrarCF_QueryInterface,
    RegistrarCF_AddRef,
    RegistrarCF_Release,
    RegistrarCF_CreateInstance,
    RegistrarCF_LockServer
};

static IClassFactory RegistrarCF = { &IRegistrarCFVtbl };

/**************************************************************
 * DllGetClassObject implementation
 */
HRESULT WINAPI ATL_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID *ppvObject)
{
    TRACE("(%s %s %p)", debugstr_guid(clsid), debugstr_guid(riid), ppvObject);

    if(IsEqualGUID(&CLSID_ATLRegistrar, clsid)) {
        *ppvObject = &RegistrarCF;
        return S_OK;
    }

    FIXME("Not supported class %s\n", debugstr_guid(clsid));
    return CLASS_E_CLASSNOTAVAILABLE;
}
