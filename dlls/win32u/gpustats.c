/*
 * Host GPU telemetry for D3DKMT statistics and performance data
 *
 * Copyright 2026 FiveM-Linux contributors
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

#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "win32u_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3dkmt);

/* NVML ABI subset, from nvml.h */
typedef int nvmlReturn_t;
typedef struct nvmlDevice_st *nvmlDevice_t;
typedef struct { unsigned int gpu, memory; } nvmlUtilization_t;
typedef struct { unsigned long long total, free, used; } nvmlMemory_t;
#define NVML_TEMPERATURE_GPU 0
#define NVML_CLOCK_MEM 2

static nvmlReturn_t (*p_nvmlInit_v2)(void);
static nvmlReturn_t (*p_nvmlDeviceGetHandleByUUID)( const char *uuid, nvmlDevice_t *device );
static nvmlReturn_t (*p_nvmlDeviceGetUtilizationRates)( nvmlDevice_t device, nvmlUtilization_t *util );
static nvmlReturn_t (*p_nvmlDeviceGetMemoryInfo)( nvmlDevice_t device, nvmlMemory_t *memory );
static nvmlReturn_t (*p_nvmlDeviceGetTemperature)( nvmlDevice_t device, int sensor, unsigned int *temp );
static nvmlReturn_t (*p_nvmlDeviceGetFanSpeed)( nvmlDevice_t device, unsigned int *speed );
static nvmlReturn_t (*p_nvmlDeviceGetPowerUsage)( nvmlDevice_t device, unsigned int *power );
static nvmlReturn_t (*p_nvmlDeviceGetEnforcedPowerLimit)( nvmlDevice_t device, unsigned int *limit );
static nvmlReturn_t (*p_nvmlDeviceGetClockInfo)( nvmlDevice_t device, int type, unsigned int *clock );
static nvmlReturn_t (*p_nvmlDeviceGetMaxClockInfo)( nvmlDevice_t device, int type, unsigned int *clock );

struct gpu_device
{
    GUID uuid;
    UINT vendor, device;
    nvmlDevice_t nvml;
    char sysfs[512];        /* DRM device directory, for drivers exposing sysfs telemetry */
    ULONGLONG last_ns;      /* time of the last utilization sample */
    ULONGLONG running_time; /* integrated busy time, in 100ns units */
};

static pthread_mutex_t gpustats_lock = PTHREAD_MUTEX_INITIALIZER;
static struct gpu_device *devices;
static UINT device_count;

static ULONGLONG monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void nvml_init_once(void)
{
    void *handle;

    if (!(handle = dlopen( "libnvidia-ml.so.1", RTLD_NOW )))
    {
        TRACE( "NVML not available: %s\n", dlerror() );
        return;
    }
#define LOAD(f) if (!(p_##f = dlsym( handle, #f ))) goto failed
    LOAD( nvmlInit_v2 );
    LOAD( nvmlDeviceGetHandleByUUID );
    LOAD( nvmlDeviceGetUtilizationRates );
    LOAD( nvmlDeviceGetMemoryInfo );
    LOAD( nvmlDeviceGetTemperature );
#undef LOAD
    p_nvmlDeviceGetFanSpeed = dlsym( handle, "nvmlDeviceGetFanSpeed" );
    p_nvmlDeviceGetPowerUsage = dlsym( handle, "nvmlDeviceGetPowerUsage" );
    p_nvmlDeviceGetEnforcedPowerLimit = dlsym( handle, "nvmlDeviceGetEnforcedPowerLimit" );
    p_nvmlDeviceGetClockInfo = dlsym( handle, "nvmlDeviceGetClockInfo" );
    p_nvmlDeviceGetMaxClockInfo = dlsym( handle, "nvmlDeviceGetMaxClockInfo" );

    if (!p_nvmlInit_v2())
    {
        TRACE( "NVML initialized.\n" );
        return;
    }
    WARN( "nvmlInit failed.\n" );

failed:
    p_nvmlInit_v2 = NULL;
    dlclose( handle );
}

static BOOL read_sysfs_ulonglong( const char *dir, const char *name, ULONGLONG *value )
{
    unsigned long long data;
    char path[640];
    FILE *file;
    BOOL ret;

    snprintf( path, sizeof(path), "%s/%s", dir, name );
    if (!(file = fopen( path, "r" ))) return FALSE;
    if ((ret = fscanf( file, "%llu", &data ) == 1)) *value = data;
    fclose( file );
    return ret;
}

/* first hwmon directory of a DRM device */
static BOOL get_hwmon_dir( const char *device_dir, char *buffer, size_t size )
{
    char path[1024];
    struct dirent *entry;
    DIR *dir;

    snprintf( path, sizeof(path), "%s/hwmon", device_dir );
    if (!(dir = opendir( path ))) return FALSE;
    while ((entry = readdir( dir )))
    {
        if (strncmp( entry->d_name, "hwmon", 5 )) continue;
        snprintf( buffer, size, "%s/%s", path, entry->d_name );
        closedir( dir );
        return TRUE;
    }
    closedir( dir );
    return FALSE;
}

static BOOL read_sysfs_hex( const char *dir, const char *name, unsigned int *value )
{
    char path[640];
    FILE *file;
    BOOL ret;

    snprintf( path, sizeof(path), "%s/%s", dir, name );
    if (!(file = fopen( path, "r" ))) return FALSE;
    ret = fscanf( file, "%x", value ) == 1;
    fclose( file );
    return ret;
}

static void find_sysfs_device( struct gpu_device *gpu )
{
    struct dirent *entry;
    unsigned int vendor, device;
    char path[sizeof(gpu->sysfs)];
    DIR *dir;

    if (!(dir = opendir( "/sys/class/drm" ))) return;
    while ((entry = readdir( dir )))
    {
        if (strncmp( entry->d_name, "card", 4 ) || strchr( entry->d_name, '-' )) continue;
        snprintf( path, sizeof(path), "/sys/class/drm/%s/device", entry->d_name );
        if (!read_sysfs_hex( path, "vendor", &vendor ) || vendor != gpu->vendor) continue;
        if (!read_sysfs_hex( path, "device", &device ) || device != gpu->device) continue;
        snprintf( gpu->sysfs, sizeof(gpu->sysfs), "%s", path );
        break;
    }
    closedir( dir );
}

static struct gpu_device *get_gpu_device( const GUID *uuid, UINT vendor, UINT device )
{
    static pthread_once_t nvml_once = PTHREAD_ONCE_INIT;
    struct gpu_device *gpu, *tmp;
    UINT i;

    for (i = 0; i < device_count; i++)
        if (IsEqualGUID( &devices[i].uuid, uuid )) return &devices[i];

    if (!(tmp = realloc( devices, (device_count + 1) * sizeof(*devices) ))) return NULL;
    devices = tmp;
    gpu = &devices[device_count++];
    memset( gpu, 0, sizeof(*gpu) );
    gpu->uuid = *uuid;
    gpu->vendor = vendor;
    gpu->device = device;
    gpu->last_ns = monotonic_ns();

    if (vendor == 0x10de)
    {
        pthread_once( &nvml_once, nvml_init_once );
        if (p_nvmlInit_v2)
        {
            const BYTE *b = (const BYTE *)uuid;
            char str[64];

            snprintf( str, sizeof(str), "GPU-%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                      b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15] );
            if (p_nvmlDeviceGetHandleByUUID( str, &gpu->nvml )) gpu->nvml = NULL;
            TRACE( "NVML device for %s: %p.\n", str, gpu->nvml );
        }
    }
    if (!gpu->nvml) find_sysfs_device( gpu );
    return gpu;
}

/* busy percentage of the main 3D engine, or -1 if unknown */
static int sample_utilization( struct gpu_device *gpu )
{
    ULONGLONG value;

    if (gpu->nvml)
    {
        nvmlUtilization_t util;
        if (!p_nvmlDeviceGetUtilizationRates( gpu->nvml, &util )) return min( util.gpu, 100 );
        return -1;
    }
    if (gpu->sysfs[0] && read_sysfs_ulonglong( gpu->sysfs, "gpu_busy_percent", &value )) return min( value, 100 );
    return -1;
}

BOOL get_gpu_stats( const GUID *uuid, UINT vendor, UINT device, struct gpu_stats *stats )
{
    struct gpu_device *gpu;
    ULONGLONG now, value;
    char hwmon[1536];
    int util;

    memset( stats, 0, sizeof(*stats) );

    pthread_mutex_lock( &gpustats_lock );
    if (!(gpu = get_gpu_device( uuid, vendor, device )))
    {
        pthread_mutex_unlock( &gpustats_lock );
        return FALSE;
    }

    /* integrate the sampled utilization into a cumulative running time */
    now = monotonic_ns();
    if ((util = sample_utilization( gpu )) >= 0)
    {
        gpu->running_time += (now - gpu->last_ns) / 100 * util / 100;
        stats->flags |= GPU_STATS_RUNNING_TIME;
    }
    gpu->last_ns = now;
    stats->running_time = gpu->running_time;

    if (gpu->nvml)
    {
        nvmlMemory_t memory;
        unsigned int temp, fan, power, limit, clock;

        if (!p_nvmlDeviceGetMemoryInfo( gpu->nvml, &memory ))
        {
            stats->vram_used = memory.used;
            stats->vram_total = memory.total;
            stats->flags |= GPU_STATS_MEMORY;
        }
        if (!p_nvmlDeviceGetTemperature( gpu->nvml, NVML_TEMPERATURE_GPU, &temp ))
        {
            stats->temperature = temp * 10;
            stats->flags |= GPU_STATS_TEMPERATURE;
        }
        /* NVML reports fan speed as a percentage of the maximum */
        if (p_nvmlDeviceGetFanSpeed && !p_nvmlDeviceGetFanSpeed( gpu->nvml, &fan )) stats->fan_percent = fan;
        if (p_nvmlDeviceGetPowerUsage && p_nvmlDeviceGetEnforcedPowerLimit &&
            !p_nvmlDeviceGetPowerUsage( gpu->nvml, &power ) &&
            !p_nvmlDeviceGetEnforcedPowerLimit( gpu->nvml, &limit ) && limit)
            stats->power = (ULONGLONG)power * 1000 / limit;
        if (p_nvmlDeviceGetClockInfo && !p_nvmlDeviceGetClockInfo( gpu->nvml, NVML_CLOCK_MEM, &clock ))
            stats->memory_frequency = clock * 1000000ull;
        if (p_nvmlDeviceGetMaxClockInfo && !p_nvmlDeviceGetMaxClockInfo( gpu->nvml, NVML_CLOCK_MEM, &clock ))
            stats->max_memory_frequency = clock * 1000000ull;
    }
    else if (gpu->sysfs[0])
    {
        if (read_sysfs_ulonglong( gpu->sysfs, "mem_info_vram_used", &stats->vram_used ) &&
            read_sysfs_ulonglong( gpu->sysfs, "mem_info_vram_total", &stats->vram_total ))
            stats->flags |= GPU_STATS_MEMORY;
        if (get_hwmon_dir( gpu->sysfs, hwmon, sizeof(hwmon) ))
        {
            if (read_sysfs_ulonglong( hwmon, "temp1_input", &value ))
            {
                stats->temperature = value / 100; /* millidegrees to decidegrees */
                stats->flags |= GPU_STATS_TEMPERATURE;
            }
            if (read_sysfs_ulonglong( hwmon, "fan1_input", &value )) stats->fan_rpm = value;
        }
    }

    pthread_mutex_unlock( &gpustats_lock );
    return TRUE;
}
