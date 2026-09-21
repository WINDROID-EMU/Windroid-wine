/*
 * ANDROIDDRV Vulkan implementation (Native Android Window / Surface)
 *
 * Copyright 2017 Roderick Colenbrander
 * Copyright 2026 Windroid Project
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <dlfcn.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "android.h"
#include "wine/debug.h"

#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

#ifdef SONAME_LIBVULKAN

#define VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR 1000008000

typedef VkFlags VkAndroidSurfaceCreateFlagsKHR;

typedef struct VkAndroidSurfaceCreateInfoKHR
{
    VkStructureType                sType;
    const void*                    pNext;
    VkAndroidSurfaceCreateFlagsKHR flags;
    struct ANativeWindow*          window;
} VkAndroidSurfaceCreateInfoKHR;

static VkResult (*pvkCreateAndroidSurfaceKHR)(VkInstance, const VkAndroidSurfaceCreateInfoKHR *, const VkAllocationCallbacks *, VkSurfaceKHR *);

static const struct vulkan_driver_funcs android_vulkan_driver_funcs;

struct android_vulkan_surface
{
    struct ANativeWindow *window;
    HWND hwnd;
};

static VkResult ANDROID_vulkan_surface_create(HWND hwnd, VkInstance instance, VkSurfaceKHR *surface, void **private)
{
    VkResult res;
    VkAndroidSurfaceCreateInfoKHR create_info_host;
    struct android_vulkan_surface *vsurf;
    struct ANativeWindow *win = NULL;

    TRACE("%p %p %p %p\n", hwnd, instance, surface, private);

    if (!(vsurf = calloc(1, sizeof(*vsurf))))
    {
        ERR("Failed to allocate vulkan surface for hwnd=%p\n", hwnd);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    win = create_ioctl_window(hwnd, FALSE, 1.0f);
    if (!win)
    {
        ERR("Failed to obtain ANativeWindow for hwnd=%p\n", hwnd);
        free(vsurf);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    vsurf->window = win;
    vsurf->hwnd = hwnd;

    create_info_host.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    create_info_host.pNext = NULL;
    create_info_host.flags = 0;
    create_info_host.window = win;

    res = pvkCreateAndroidSurfaceKHR(instance, &create_info_host, NULL, surface);
    if (res != VK_SUCCESS)
    {
        ERR("Failed to create native Android Vulkan surface, res=%d\n", res);
        release_ioctl_window(win);
        free(vsurf);
        return res;
    }

    *private = vsurf;
    TRACE("Created Android Vulkan surface=0x%s, private=%p\n", wine_dbgstr_longlong(*surface), *private);
    return VK_SUCCESS;
}

static void ANDROID_vulkan_surface_destroy(HWND hwnd, void *private)
{
    struct android_vulkan_surface *vsurf = private;

    TRACE("%p %p\n", hwnd, private);

    if (vsurf)
    {
        if (vsurf->window)
        {
            release_ioctl_window(vsurf->window);
        }
        free(vsurf);
    }
}

static void ANDROID_vulkan_surface_detach(HWND hwnd, void *private)
{
    TRACE("%p %p\n", hwnd, private);
}

static void ANDROID_vulkan_surface_update(HWND hwnd, void *private)
{
    TRACE("%p %p\n", hwnd, private);
}

static void ANDROID_vulkan_surface_presented(HWND hwnd, void *private, VkResult result)
{
    (void)hwnd;
    (void)private;
    (void)result;
}

static VkBool32 ANDROID_vkGetPhysicalDeviceWin32PresentationSupportKHR(VkPhysicalDevice phys_dev, uint32_t index)
{
    TRACE("%p %u\n", phys_dev, index);
    // No Android, apresentação Vulkan via ANativeWindow é suportada por padrão
    return VK_TRUE;
}

static const char *ANDROID_get_host_surface_extension(void)
{
    return "VK_KHR_android_surface";
}

static const struct vulkan_driver_funcs android_vulkan_driver_funcs =
{
    .p_vulkan_surface_create = ANDROID_vulkan_surface_create,
    .p_vulkan_surface_destroy = ANDROID_vulkan_surface_destroy,
    .p_vulkan_surface_detach = ANDROID_vulkan_surface_detach,
    .p_vulkan_surface_update = ANDROID_vulkan_surface_update,
    .p_vulkan_surface_presented = ANDROID_vulkan_surface_presented,

    .p_vkGetPhysicalDeviceWin32PresentationSupportKHR = ANDROID_vkGetPhysicalDeviceWin32PresentationSupportKHR,
    .p_get_host_surface_extension = ANDROID_get_host_surface_extension,
};

UINT ANDROID_VulkanInit(UINT version, void *vulkan_handle, const struct vulkan_driver_funcs **driver_funcs)
{
    if (version != WINE_VULKAN_DRIVER_VERSION)
    {
        ERR("version mismatch, win32u wants %u but driver has %u\n", version, WINE_VULKAN_DRIVER_VERSION);
        return STATUS_INVALID_PARAMETER;
    }

#define LOAD_FUNCPTR(f) if (!(p##f = dlsym(vulkan_handle, #f))) return STATUS_PROCEDURE_NOT_FOUND;
    LOAD_FUNCPTR(vkCreateAndroidSurfaceKHR);
#undef LOAD_FUNCPTR

    *driver_funcs = &android_vulkan_driver_funcs;
    TRACE("Vulkan initialized successfully with VK_KHR_android_surface\n");
    return STATUS_SUCCESS;
}

#else /* SONAME_LIBVULKAN */

UINT ANDROID_VulkanInit(UINT version, void *vulkan_handle, const struct vulkan_driver_funcs **driver_funcs)
{
    ERR("Wine was built without Vulkan support.\n");
    return STATUS_NOT_IMPLEMENTED;
}

#endif /* SONAME_LIBVULKAN */
