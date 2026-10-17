/*
 * Performance Data Helper (pdh.dll)
 *
 * Copyright 2007 Andrey Turkin
 * Copyright 2007 Hans Leidekker
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

#include <stdarg.h>
#include <stdlib.h>
#include <math.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winternl.h"
#include "ddk/d3dkmthk.h"

#include "pdh.h"
#include "pdhmsg.h"
#include "winperf.h"

#include "wine/debug.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(pdh);

static CRITICAL_SECTION pdh_handle_cs;
static CRITICAL_SECTION_DEBUG pdh_handle_cs_debug =
{
    0, 0, &pdh_handle_cs,
    { &pdh_handle_cs_debug.ProcessLocksList,
      &pdh_handle_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": pdh_handle_cs") }
};
static CRITICAL_SECTION pdh_handle_cs = { &pdh_handle_cs_debug, -1, 0, 0, 0, 0 };

static inline WCHAR *pdh_strdup_aw( const char *src )
{
    int len;
    WCHAR *dst;

    if (!src) return NULL;
    len = MultiByteToWideChar( CP_ACP, 0, src, -1, NULL, 0 );
    if ((dst = malloc( len * sizeof(WCHAR) ))) MultiByteToWideChar( CP_ACP, 0, src, -1, dst, len );
    return dst;
}

union value
{
    LONG     longvalue;
    double   doublevalue;
    LONGLONG largevalue;
};

struct counter
{
    DWORD           magic;                          /* signature */
    struct list     entry;                          /* list entry */
    WCHAR          *path;                           /* identifier */
    DWORD           type;                           /* counter type */
    DWORD           status;                         /* update status */
    LONG            scale;                          /* scale factor */
    LONG            defaultscale;                   /* default scale factor */
    DWORD_PTR       user;                           /* user data */
    DWORD_PTR       queryuser;                      /* query user data */
    LONGLONG        base;                           /* samples per second */
    FILETIME        stamp;                          /* time stamp */
    void (CALLBACK *collect)( struct counter * );   /* collect callback */
    union value     one;                            /* first value */
    union value     two;                            /* second value */
    PDH_RAW_COUNTER raw[2];                         /* previous and current raw sample */
    struct instance *instances;                     /* wildcard counter instances */
    DWORD           instance_count;
};

struct instance
{
    WCHAR          *name;
    PDH_RAW_COUNTER raw[2];                         /* previous and current raw sample */
};

#define PDH_MAGIC_COUNTER   0x50444831 /* 'PDH1' */

static struct counter *create_counter( void )
{
    struct counter *counter;

    if ((counter = calloc( 1, sizeof(struct counter) )))
    {
        counter->magic = PDH_MAGIC_COUNTER;
        return counter;
    }
    return NULL;
}

static void free_instances( struct instance *instances, DWORD count )
{
    DWORD i;

    for (i = 0; i < count; i++) free( instances[i].name );
    free( instances );
}

static void destroy_counter( struct counter *counter )
{
    /* Ensure compiler doesn't optimize out the assignment with 0. */
    SecureZeroMemory( &counter->magic, sizeof( counter->magic ) );
    free_instances( counter->instances, counter->instance_count );
    free( counter->path );
    free( counter );
}

#define PDH_MAGIC_QUERY     0x50444830 /* 'PDH0' */

struct query
{
    DWORD       magic;      /* signature */
    DWORD_PTR   user;       /* user data */
    HANDLE      thread;     /* collect thread */
    DWORD       interval;   /* collect interval */
    HANDLE      wait;       /* wait event */
    HANDLE      stop;       /* stop event */
    struct list counters;   /* counter list */
};

static struct query *create_query( void )
{
    struct query *query;

    if ((query = calloc( 1, sizeof(struct query) )))
    {
        query->magic = PDH_MAGIC_QUERY;
        list_init( &query->counters );
        return query;
    }
    return NULL;
}

static void destroy_query( struct query *query )
{
    /* Ensure compiler doesn't optimize out the assignment with 0. */
    SecureZeroMemory( &query->magic, sizeof( query->magic ) );
    free( query );
}

struct source
{
    DWORD           index;                          /* name index */
    const WCHAR    *path;                           /* identifier */
    void (CALLBACK *collect)( struct counter * );   /* collect callback */
    DWORD           type;                           /* counter type */
    LONG            scale;                          /* default scale factor */
    LONGLONG        base;                           /* samples per second */
};

static LONGLONG current_time(void)
{
    FILETIME ft;

    GetSystemTimePreciseAsFileTime( &ft );
    return ((LONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static void set_raw_value( PDH_RAW_COUNTER raw[2], LONGLONG first, LONGLONG second, LONGLONG time )
{
    raw[0] = raw[1];
    raw[1].CStatus                  = PDH_CSTATUS_VALID_DATA;
    raw[1].TimeStamp.dwLowDateTime  = time;
    raw[1].TimeStamp.dwHighDateTime = time >> 32;
    raw[1].FirstValue               = first;
    raw[1].SecondValue              = second;
    raw[1].MultiCount               = 1;
}

/* average idle time of all processors, in 100ns units */
static void CALLBACK collect_processor_time( struct counter *counter )
{
    SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION *info;
    SYSTEM_BASIC_INFORMATION basic;
    LONGLONG idle = 0, time = current_time();
    ULONG i;

    counter->status = PDH_CSTATUS_INVALID_DATA;
    if (NtQuerySystemInformation( SystemBasicInformation, &basic, sizeof(basic), NULL )) return;
    if (!(info = calloc( basic.NumberOfProcessors, sizeof(*info) ))) return;
    if (!NtQuerySystemInformation( SystemProcessorPerformanceInformation, info,
                                   basic.NumberOfProcessors * sizeof(*info), NULL ))
    {
        for (i = 0; i < basic.NumberOfProcessors; i++) idle += info[i].IdleTime.QuadPart;
        set_raw_value( counter->raw, idle / basic.NumberOfProcessors, 0, time );
        counter->status = PDH_CSTATUS_VALID_DATA;
    }
    free( info );
}

static void CALLBACK collect_uptime( struct counter *counter )
{
    counter->two.largevalue = GetTickCount64();
    counter->status = PDH_CSTATUS_VALID_DATA;
}

static void update_instance( struct instance **instances, DWORD *count, struct instance *old, DWORD old_count,
                             const WCHAR *name, LONGLONG first, LONGLONG time )
{
    struct instance *tmp, *inst;
    DWORD i;

    if (!(tmp = realloc( *instances, (*count + 1) * sizeof(**instances) ))) return;
    *instances = tmp;
    inst = &tmp[*count];
    memset( inst, 0, sizeof(*inst) );
    if (!(inst->name = wcsdup( name ))) return;
    (*count)++;

    for (i = 0; i < old_count; i++)
    {
        if (wcscmp( old[i].name, name )) continue;
        inst->raw[1] = old[i].raw[1];
        break;
    }
    set_raw_value( inst->raw, first, 0, time );
}

enum gpu_counter
{
    GPU_ENGINE_RUNNING_TIME,
    GPU_ADAPTER_DEDICATED_USAGE,
};

static void collect_gpu( struct counter *counter, enum gpu_counter which )
{
    D3DKMT_ENUMADAPTERS2 enum_adapters = {0};
    struct instance *instances = NULL;
    D3DKMT_ADAPTERINFO *adapters;
    LONGLONG time = current_time();
    DWORD count = 0;
    WCHAR name[96];
    ULONG i;

    counter->status = PDH_CSTATUS_INVALID_DATA;
    if (D3DKMTEnumAdapters2( &enum_adapters )) return;
    if (!(adapters = calloc( enum_adapters.NumAdapters, sizeof(*adapters) ))) return;
    enum_adapters.pAdapters = adapters;
    if (D3DKMTEnumAdapters2( &enum_adapters ))
    {
        free( adapters );
        return;
    }

    for (i = 0; i < enum_adapters.NumAdapters; i++)
    {
        D3DKMT_QUERYSTATISTICS stats = {0};
        D3DKMT_CLOSEADAPTER close = {adapters[i].hAdapter};
        LUID luid = adapters[i].AdapterLuid;

        D3DKMTCloseAdapter( &close );
        stats.AdapterLuid = luid;

        if (which == GPU_ENGINE_RUNNING_TIME)
        {
            stats.Type = D3DKMT_QUERYSTATISTICS_NODE;
            stats.QueryNode.NodeId = 0;
            if (D3DKMTQueryStatistics( &stats )) continue;
            /* only system-wide engine time is known, report it for the calling process */
            swprintf( name, ARRAY_SIZE(name), L"pid_%lu_luid_0x%08lX_0x%08lX_phys_0_eng_0_engtype_3D",
                      GetCurrentProcessId(), luid.HighPart, luid.LowPart );
            update_instance( &instances, &count, counter->instances, counter->instance_count, name,
                             stats.QueryResult.NodeInformation.GlobalInformation.RunningTime.QuadPart, time );
        }
        else
        {
            stats.Type = D3DKMT_QUERYSTATISTICS_SEGMENT;
            stats.QuerySegment.SegmentId = 0;
            if (D3DKMTQueryStatistics( &stats )) continue;
            swprintf( name, ARRAY_SIZE(name), L"luid_0x%08lX_0x%08lX_phys_0", luid.HighPart, luid.LowPart );
            update_instance( &instances, &count, counter->instances, counter->instance_count, name,
                             stats.QueryResult.SegmentInformation.BytesResident, time );
        }
    }
    free( adapters );

    free_instances( counter->instances, counter->instance_count );
    counter->instances = instances;
    counter->instance_count = count;
    counter->status = PDH_CSTATUS_VALID_DATA;
}

static void CALLBACK collect_gpu_engine( struct counter *counter )
{
    collect_gpu( counter, GPU_ENGINE_RUNNING_TIME );
}

static void CALLBACK collect_gpu_memory( struct counter *counter )
{
    collect_gpu( counter, GPU_ADAPTER_DEDICATED_USAGE );
}

#define TYPE_PROCESSOR_TIME PERF_100NSEC_TIMER_INV

#define TYPE_UPTIME \
    (PERF_SIZE_LARGE | PERF_TYPE_COUNTER | PERF_COUNTER_ELAPSED | PERF_OBJECT_TIMER | PERF_DISPLAY_SECONDS)

/* counter source registry, index 0 means no registered name index */
static const struct source counter_sources[] =
{
    { 6,   L"\\Processor(_Total)\\% Processor Time",                collect_processor_time, TYPE_PROCESSOR_TIME,         0, 10000000 },
    { 674, L"\\System\\System Up Time",                              collect_uptime,         TYPE_UPTIME,                -3, 1000 },
    { 0,   L"\\Processor Information(_Total)\\% Processor Time",    collect_processor_time, TYPE_PROCESSOR_TIME,         0, 10000000 },
    { 0,   L"\\Processor Information(_Total)\\% Processor Utility", collect_processor_time, TYPE_PROCESSOR_TIME,         0, 10000000 },
    { 0,   L"\\GPU Engine(*)\\Utilization Percentage",              collect_gpu_engine,     PERF_100NSEC_TIMER,          0, 10000000 },
    { 0,   L"\\GPU Engine(*)\\Running Time",                        collect_gpu_engine,     PERF_COUNTER_LARGE_RAWCOUNT, 0, 10000000 },
    { 0,   L"\\GPU Adapter Memory(*)\\Dedicated Usage",             collect_gpu_memory,     PERF_COUNTER_LARGE_RAWCOUNT, 0, 1 },
};

static BOOL is_wildcard_source( const struct counter *counter )
{
    return wcsstr( counter->path, L"(*)" ) != NULL;
}

static BOOL is_local_machine( const WCHAR *name, DWORD len )
{
    WCHAR buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD buflen = ARRAY_SIZE(buf);

    if (!GetComputerNameW( buf, &buflen )) return FALSE;
    return len == buflen && !wcsnicmp( name, buf, buflen );
}

static BOOL pdh_match_path( LPCWSTR fullpath, LPCWSTR path )
{
    const WCHAR *p;

    if (path[0] == '\\' && path[1] == '\\' && (p = wcschr( path + 2, '\\' )) &&
        is_local_machine( path + 2, p - path - 2 ))
    {
        path += p - path;
    }
    if (wcschr( path, '\\' )) p = fullpath;
    else p = wcsrchr( fullpath, '\\' ) + 1;
    return !wcscmp( p, path );
}

/***********************************************************************
 *              PdhAddCounterA   (PDH.@)
 */
PDH_STATUS WINAPI PdhAddCounterA( PDH_HQUERY query, LPCSTR path,
                                  DWORD_PTR userdata, PDH_HCOUNTER *counter )
{
    PDH_STATUS ret;
    WCHAR *pathW;

    TRACE("%p %s %Ix %p\n", query, debugstr_a(path), userdata, counter);

    if (!path) return PDH_INVALID_ARGUMENT;

    if (!(pathW = pdh_strdup_aw( path )))
        return PDH_MEMORY_ALLOCATION_FAILURE;

    ret = PdhAddCounterW( query, pathW, userdata, counter );

    free( pathW );
    return ret;
}

/***********************************************************************
 *              PdhAddCounterW   (PDH.@)
 */
PDH_STATUS WINAPI PdhAddCounterW( PDH_HQUERY hquery, LPCWSTR path,
                                  DWORD_PTR userdata, PDH_HCOUNTER *hcounter )
{
    struct query *query = hquery;
    struct counter *counter;
    unsigned int i;

    TRACE("%p %s %Ix %p\n", hquery, debugstr_w(path), userdata, hcounter);

    if (!path  || !hcounter) return PDH_INVALID_ARGUMENT;

    EnterCriticalSection( &pdh_handle_cs );
    if (!query || query->magic != PDH_MAGIC_QUERY)
    {
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_INVALID_HANDLE;
    }

    *hcounter = NULL;
    for (i = 0; i < ARRAY_SIZE(counter_sources); i++)
    {
        if (pdh_match_path( counter_sources[i].path, path ))
        {
            if ((counter = create_counter()))
            {
                counter->path         = wcsdup( counter_sources[i].path );
                counter->collect      = counter_sources[i].collect;
                counter->type         = counter_sources[i].type;
                counter->defaultscale = counter_sources[i].scale;
                counter->base         = counter_sources[i].base;
                counter->queryuser    = query->user;
                counter->user         = userdata;

                list_add_tail( &query->counters, &counter->entry );
                *hcounter = counter;

                LeaveCriticalSection( &pdh_handle_cs );
                return ERROR_SUCCESS;
            }
            LeaveCriticalSection( &pdh_handle_cs );
            return PDH_MEMORY_ALLOCATION_FAILURE;
        }
    }
    LeaveCriticalSection( &pdh_handle_cs );
    return PDH_CSTATUS_NO_COUNTER;
}

/***********************************************************************
 *              PdhAddEnglishCounterA   (PDH.@)
 */
PDH_STATUS WINAPI PdhAddEnglishCounterA( PDH_HQUERY query, LPCSTR path,
                                         DWORD_PTR userdata, PDH_HCOUNTER *counter )
{
    TRACE("%p %s %Ix %p\n", query, debugstr_a(path), userdata, counter);

    if (!counter) return PDH_INVALID_ARGUMENT;
    if (!query) return PDH_INVALID_HANDLE;
    return PdhAddCounterA( query, path, userdata, counter );
}

/***********************************************************************
 *              PdhAddEnglishCounterW   (PDH.@)
 */
PDH_STATUS WINAPI PdhAddEnglishCounterW( PDH_HQUERY query, LPCWSTR path,
                                         DWORD_PTR userdata, PDH_HCOUNTER *counter )
{
    TRACE("%p %s %Ix %p\n", query, debugstr_w(path), userdata, counter);

    if (!counter) return PDH_INVALID_ARGUMENT;
    if (!query) return PDH_INVALID_HANDLE;
    return PdhAddCounterW( query, path, userdata, counter );
}

static LONGLONG raw_time( const PDH_RAW_COUNTER *raw )
{
    return ((LONGLONG)raw->TimeStamp.dwHighDateTime << 32) | raw->TimeStamp.dwLowDateTime;
}

/* compute a value from the current and previous raw samples of a counter type */
static PDH_STATUS calculate_raw( DWORD type, DWORD format, LONG scale, const PDH_RAW_COUNTER *cur,
                                 const PDH_RAW_COUNTER *prev, PDH_FMT_COUNTERVALUE *value )
{
    double result;
    LONGLONG delta;

    if (!cur || cur->CStatus != PDH_CSTATUS_VALID_DATA) return PDH_INVALID_DATA;

    switch (type)
    {
    case PERF_COUNTER_RAWCOUNT:
    case PERF_COUNTER_LARGE_RAWCOUNT:
        result = cur->FirstValue;
        break;

    case PERF_100NSEC_TIMER:
    case PERF_100NSEC_TIMER_INV:
        if (!prev || prev->CStatus != PDH_CSTATUS_VALID_DATA) return PDH_INVALID_DATA;
        if ((delta = raw_time( cur ) - raw_time( prev )) <= 0) return PDH_CALC_NEGATIVE_DENOMINATOR;
        result = (double)(cur->FirstValue - prev->FirstValue) / delta;
        if (type == PERF_100NSEC_TIMER_INV) result = 1.0 - result;
        result *= 100.0;
        if (result < 0.0) result = 0.0;
        if (!(format & PDH_FMT_NOCAP100) && result > 100.0) result = 100.0;
        break;

    default:
        FIXME( "counter type %#lx not supported\n", type );
        return PDH_INVALID_ARGUMENT;
    }

    if (!(format & PDH_FMT_NOSCALE)) result *= pow( 10, scale );
    if (format & PDH_FMT_1000) result *= 1000;

    if (format & PDH_FMT_LONG) value->longValue = result;
    else if (format & PDH_FMT_LARGE) value->largeValue = result;
    else if (format & PDH_FMT_DOUBLE) value->doubleValue = result;
    else
    {
        WARN( "unknown format %lx\n", format );
        return PDH_INVALID_ARGUMENT;
    }
    value->CStatus = ERROR_SUCCESS;
    return ERROR_SUCCESS;
}

static BOOL uses_raw_samples( const struct counter *counter )
{
    return counter->type != TYPE_UPTIME;
}

/* caller must hold counter lock */
static PDH_STATUS format_value( struct counter *counter, DWORD format, union value *raw1,
                                union value *raw2, PDH_FMT_COUNTERVALUE *value )
{
    LONG factor;

    factor = counter->scale ? counter->scale : counter->defaultscale;
    if (format & PDH_FMT_LONG)
    {
        if (format & PDH_FMT_1000) value->longValue = raw2->longvalue * 1000;
        else value->longValue = raw2->longvalue * pow( 10, factor );
    }
    else if (format & PDH_FMT_LARGE)
    {
        if (format & PDH_FMT_1000) value->largeValue = raw2->largevalue * 1000;
        else value->largeValue = raw2->largevalue * pow( 10, factor );
    }
    else if (format & PDH_FMT_DOUBLE)
    {
        if (format & PDH_FMT_1000) value->doubleValue = raw2->doublevalue * 1000;
        else value->doubleValue = raw2->doublevalue * pow( 10, factor );
    }
    else
    {
        WARN("unknown format %lx\n", format);
        return PDH_INVALID_ARGUMENT;
    }
    return ERROR_SUCCESS;
}

/***********************************************************************
 *              PdhCalculateCounterFromRawValue   (PDH.@)
 */
PDH_STATUS WINAPI PdhCalculateCounterFromRawValue( PDH_HCOUNTER handle, DWORD format,
                                                   PPDH_RAW_COUNTER raw1, PPDH_RAW_COUNTER raw2,
                                                   PPDH_FMT_COUNTERVALUE value )
{
    PDH_STATUS ret;
    struct counter *counter = handle;

    TRACE("%p 0x%08lx %p %p %p\n", handle, format, raw1, raw2, value);

    if (!value) return PDH_INVALID_ARGUMENT;

    EnterCriticalSection( &pdh_handle_cs );
    if (!counter || counter->magic != PDH_MAGIC_COUNTER)
    {
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_INVALID_HANDLE;
    }

    if (uses_raw_samples( counter ))
        ret = calculate_raw( counter->type, format, counter->scale ? counter->scale : counter->defaultscale,
                             raw1, raw2, value );
    else
        ret = format_value( counter, format, (union value *)&raw1->SecondValue,
                                             (union value *)&raw2->SecondValue, value );

    LeaveCriticalSection( &pdh_handle_cs );
    return ret;
}


/***********************************************************************
 *              PdhCloseQuery   (PDH.@)
 */
PDH_STATUS WINAPI PdhCloseQuery( PDH_HQUERY handle )
{
    struct query *query = handle;
    struct list *item, *next;

    TRACE("%p\n", handle);

    EnterCriticalSection( &pdh_handle_cs );
    if (!query || query->magic != PDH_MAGIC_QUERY)
    {
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_INVALID_HANDLE;
    }

    if (query->thread)
    {
        HANDLE thread = query->thread;
        SetEvent( query->stop );
        LeaveCriticalSection( &pdh_handle_cs );

        WaitForSingleObject( thread, INFINITE );

        EnterCriticalSection( &pdh_handle_cs );
        if (query->magic != PDH_MAGIC_QUERY)
        {
            LeaveCriticalSection( &pdh_handle_cs );
            return ERROR_SUCCESS;
        }
        CloseHandle( query->stop );
        CloseHandle( query->thread );
        query->thread = NULL;
    }

    LIST_FOR_EACH_SAFE( item, next, &query->counters )
    {
        struct counter *counter = LIST_ENTRY( item, struct counter, entry );

        list_remove( &counter->entry );
        destroy_counter( counter );
    }

    destroy_query( query );

    LeaveCriticalSection( &pdh_handle_cs );
    return ERROR_SUCCESS;
}

/* caller must hold query lock */
static void collect_query_data( struct query *query )
{
    struct list *item;

    LIST_FOR_EACH( item, &query->counters )
    {
        SYSTEMTIME time;
        struct counter *counter = LIST_ENTRY( item, struct counter, entry );

        counter->collect( counter );

        GetLocalTime( &time );
        SystemTimeToFileTime( &time, &counter->stamp );
    }
}

/***********************************************************************
 *              PdhCollectQueryData   (PDH.@)
 */
PDH_STATUS WINAPI PdhCollectQueryData( PDH_HQUERY handle )
{
    struct query *query = handle;

    TRACE("%p\n", handle);

    EnterCriticalSection( &pdh_handle_cs );
    if (!query || query->magic != PDH_MAGIC_QUERY)
    {
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_INVALID_HANDLE;
    }

    if (list_empty( &query->counters ))
    {
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_NO_DATA;
    }

    collect_query_data( query );

    LeaveCriticalSection( &pdh_handle_cs );
    return ERROR_SUCCESS;
}

static DWORD CALLBACK collect_query_thread( void *arg )
{
    struct query *query = arg;
    DWORD interval = query->interval;
    HANDLE stop = query->stop;

    for (;;)
    {
        if (WaitForSingleObject( stop, interval ) != WAIT_TIMEOUT) ExitThread( 0 );

        EnterCriticalSection( &pdh_handle_cs );
        if (query->magic != PDH_MAGIC_QUERY)
        {
            LeaveCriticalSection( &pdh_handle_cs );
            ExitThread( PDH_INVALID_HANDLE );
        }

        collect_query_data( query );

        if (!SetEvent( query->wait ))
        {
            LeaveCriticalSection( &pdh_handle_cs );
            ExitThread( 0 );
        }
        LeaveCriticalSection( &pdh_handle_cs );
    }
}

/***********************************************************************
 *              PdhCollectQueryDataEx   (PDH.@)
 */
PDH_STATUS WINAPI PdhCollectQueryDataEx( PDH_HQUERY handle, DWORD interval, HANDLE event )
{
    PDH_STATUS ret;
    struct query *query = handle;

    TRACE("%p %ld %p\n", handle, interval, event);

    EnterCriticalSection( &pdh_handle_cs );
    if (!query || query->magic != PDH_MAGIC_QUERY)
    {
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_INVALID_HANDLE;
    }
    if (list_empty( &query->counters ))
    {
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_NO_DATA;
    }
    if (query->thread)
    {
        HANDLE thread = query->thread;
        SetEvent( query->stop );
        LeaveCriticalSection( &pdh_handle_cs );

        WaitForSingleObject( thread, INFINITE );

        EnterCriticalSection( &pdh_handle_cs );
        if (query->magic != PDH_MAGIC_QUERY)
        {
            LeaveCriticalSection( &pdh_handle_cs );
            return PDH_INVALID_HANDLE;
        }
        CloseHandle( query->thread );
        query->thread = NULL;
    }
    else if (!(query->stop = CreateEventW( NULL, FALSE, FALSE, NULL )))
    {
        ret = GetLastError();
        LeaveCriticalSection( &pdh_handle_cs );
        return ret;
    }
    query->wait = event;
    query->interval = interval * 1000;
    if (!(query->thread = CreateThread( NULL, 0, collect_query_thread, query, 0, NULL )))
    {
        ret = GetLastError();
        CloseHandle( query->stop );

        LeaveCriticalSection( &pdh_handle_cs );
        return ret;
    }

    LeaveCriticalSection( &pdh_handle_cs );
    return ERROR_SUCCESS;
}

/***********************************************************************
 *              PdhCollectQueryDataWithTime   (PDH.@)
 */
PDH_STATUS WINAPI PdhCollectQueryDataWithTime( PDH_HQUERY handle, LONGLONG *timestamp )
{
    struct query *query = handle;
    struct counter *counter;
    struct list *item;

    TRACE("%p %p\n", handle, timestamp);

    if (!timestamp) return PDH_INVALID_ARGUMENT;

    EnterCriticalSection( &pdh_handle_cs );
    if (!query || query->magic != PDH_MAGIC_QUERY)
    {
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_INVALID_HANDLE;
    }
    if (list_empty( &query->counters ))
    {
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_NO_DATA;
    }

    collect_query_data( query );

    item = list_head( &query->counters );
    counter = LIST_ENTRY( item, struct counter, entry );

    *timestamp = ((LONGLONG)counter->stamp.dwHighDateTime << 32) | counter->stamp.dwLowDateTime;

    LeaveCriticalSection( &pdh_handle_cs );
    return ERROR_SUCCESS;
}

/***********************************************************************
 *              PdhExpandWildCardPathA   (PDH.@)
 */
PDH_STATUS WINAPI PdhExpandWildCardPathA( LPCSTR szDataSource, LPCSTR szWildCardPath, LPSTR mszExpandedPathList, LPDWORD pcchPathListLength, DWORD dwFlags )
{
    FIXME("%s, %s, %p, %p, 0x%lx: stub\n", debugstr_a(szDataSource), debugstr_a(szWildCardPath), mszExpandedPathList, pcchPathListLength, dwFlags);
    return PDH_NOT_IMPLEMENTED;
}

/***********************************************************************
 *              PdhExpandWildCardPathW   (PDH.@)
 */
PDH_STATUS WINAPI PdhExpandWildCardPathW( LPCWSTR szDataSource, LPCWSTR szWildCardPath, LPWSTR mszExpandedPathList, LPDWORD pcchPathListLength, DWORD dwFlags )
{
    FIXME("%s, %s, %p, %p, 0x%lx: stub\n", debugstr_w(szDataSource), debugstr_w(szWildCardPath), mszExpandedPathList, pcchPathListLength, dwFlags);
    return PDH_NOT_IMPLEMENTED;
}

/***********************************************************************
 *              PdhExpandCounterPathA   (PDH.@)
 */
PDH_STATUS WINAPI PdhExpandCounterPathA( LPCSTR szWildCardPath, LPSTR mszExpandedPathList, LPDWORD pcchPathListLength )
{
    FIXME("%s, %p, %p: stub\n", debugstr_a(szWildCardPath), mszExpandedPathList, pcchPathListLength);
    return PdhExpandWildCardPathA(NULL, szWildCardPath, mszExpandedPathList, pcchPathListLength, 0);
}

/***********************************************************************
 *              PdhExpandCounterPathW   (PDH.@)
 */
PDH_STATUS WINAPI PdhExpandCounterPathW( LPCWSTR szWildCardPath, LPWSTR mszExpandedPathList, LPDWORD pcchPathListLength )
{
    FIXME("%s, %p, %p: stub\n", debugstr_w(szWildCardPath), mszExpandedPathList, pcchPathListLength);
    return PdhExpandWildCardPathW(NULL, szWildCardPath, mszExpandedPathList, pcchPathListLength, 0);
}

/***********************************************************************
 *              PdhGetCounterInfoA   (PDH.@)
 */
PDH_STATUS WINAPI PdhGetCounterInfoA( PDH_HCOUNTER handle, BOOLEAN text, LPDWORD size, PPDH_COUNTER_INFO_A info )
{
    struct counter *counter = handle;

    TRACE("%p %d %p %p\n", handle, text, size, info);

    EnterCriticalSection( &pdh_handle_cs );
    if (!counter || counter->magic != PDH_MAGIC_COUNTER)
    {
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_INVALID_HANDLE;
    }
    if (!size)
    {
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_INVALID_ARGUMENT;
    }
    if (*size < sizeof(PDH_COUNTER_INFO_A))
    {
        *size = sizeof(PDH_COUNTER_INFO_A);
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_MORE_DATA;
    }

    memset( info, 0, sizeof(PDH_COUNTER_INFO_A) );

    info->dwType          = counter->type;
    info->CStatus         = counter->status;
    info->lScale          = counter->scale;
    info->lDefaultScale   = counter->defaultscale;
    info->dwUserData      = counter->user;
    info->dwQueryUserData = counter->queryuser;

    *size = sizeof(PDH_COUNTER_INFO_A);

    LeaveCriticalSection( &pdh_handle_cs );
    return ERROR_SUCCESS;
}

/***********************************************************************
 *              PdhGetCounterInfoW   (PDH.@)
 */
PDH_STATUS WINAPI PdhGetCounterInfoW( PDH_HCOUNTER handle, BOOLEAN text, LPDWORD size, PPDH_COUNTER_INFO_W info )
{
    struct counter *counter = handle;

    TRACE("%p %d %p %p\n", handle, text, size, info);

    EnterCriticalSection( &pdh_handle_cs );
    if (!counter || counter->magic != PDH_MAGIC_COUNTER)
    {
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_INVALID_HANDLE;
    }
    if (!size)
    {
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_INVALID_ARGUMENT;
    }
    if (*size < sizeof(PDH_COUNTER_INFO_W))
    {
        *size = sizeof(PDH_COUNTER_INFO_W);
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_MORE_DATA;
    }

    memset( info, 0, sizeof(PDH_COUNTER_INFO_W) );

    info->dwType          = counter->type;
    info->CStatus         = counter->status;
    info->lScale          = counter->scale;
    info->lDefaultScale   = counter->defaultscale;
    info->dwUserData      = counter->user;
    info->dwQueryUserData = counter->queryuser;

    *size = sizeof(PDH_COUNTER_INFO_W);

    LeaveCriticalSection( &pdh_handle_cs );
    return ERROR_SUCCESS;
}

/***********************************************************************
 *              PdhGetCounterTimeBase   (PDH.@)
 */
PDH_STATUS WINAPI PdhGetCounterTimeBase( PDH_HCOUNTER handle, LONGLONG *base )
{
    struct counter *counter = handle;

    TRACE("%p %p\n", handle, base);

    if (!base) return PDH_INVALID_ARGUMENT;

    EnterCriticalSection( &pdh_handle_cs );
    if (!counter || counter->magic != PDH_MAGIC_COUNTER)
    {
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_INVALID_HANDLE;
    }

    *base = counter->base;

    LeaveCriticalSection( &pdh_handle_cs );
    return ERROR_SUCCESS;
}

/***********************************************************************
 *              PdhGetDllVersion   (PDH.@)
 */
PDH_STATUS WINAPI PdhGetDllVersion( LPDWORD version )
{
    if (!version)
        return PDH_INVALID_ARGUMENT;

    *version = PDH_VERSION;

    return ERROR_SUCCESS;
}

/***********************************************************************
 *              PdhGetFormattedCounterValue   (PDH.@)
 */
PDH_STATUS WINAPI PdhGetFormattedCounterValue( PDH_HCOUNTER handle, DWORD format,
                                               LPDWORD type, PPDH_FMT_COUNTERVALUE value )
{
    PDH_STATUS ret;
    struct counter *counter = handle;

    TRACE("%p %lx %p %p\n", handle, format, type, value);

    if (!value) return PDH_INVALID_ARGUMENT;

    EnterCriticalSection( &pdh_handle_cs );
    if (!counter || counter->magic != PDH_MAGIC_COUNTER)
    {
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_INVALID_HANDLE;
    }
    if (counter->status)
    {
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_INVALID_DATA;
    }
    if (uses_raw_samples( counter ))
    {
        if (is_wildcard_source( counter )) ret = PDH_INVALID_HANDLE;
        else ret = calculate_raw( counter->type, format, counter->scale ? counter->scale : counter->defaultscale,
                                  &counter->raw[1], &counter->raw[0], value );
        if (!ret && type) *type = counter->type;
    }
    else if (!(ret = format_value( counter, format, &counter->one, &counter->two, value )))
    {
        value->CStatus = ERROR_SUCCESS;
        if (type) *type = counter->type;
    }

    LeaveCriticalSection( &pdh_handle_cs );
    return ret;
}

/***********************************************************************
 *              PdhGetRawCounterValue   (PDH.@)
 */
PDH_STATUS WINAPI PdhGetRawCounterValue( PDH_HCOUNTER handle, LPDWORD type,
                                         PPDH_RAW_COUNTER value )
{
    struct counter *counter = handle;

    TRACE("%p %p %p\n", handle, type, value);

    if (!value) return PDH_INVALID_ARGUMENT;

    EnterCriticalSection( &pdh_handle_cs );
    if (!counter || counter->magic != PDH_MAGIC_COUNTER)
    {
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_INVALID_HANDLE;
    }

    if (uses_raw_samples( counter ) && counter->raw[1].CStatus == PDH_CSTATUS_VALID_DATA)
    {
        *value = counter->raw[1];
    }
    else
    {
        value->CStatus                  = counter->status;
        value->TimeStamp.dwLowDateTime  = counter->stamp.dwLowDateTime;
        value->TimeStamp.dwHighDateTime = counter->stamp.dwHighDateTime;
        value->FirstValue               = counter->one.largevalue;
        value->SecondValue              = counter->two.largevalue;
        value->MultiCount               = 1; /* FIXME */
    }

    if (type) *type = counter->type;

    LeaveCriticalSection( &pdh_handle_cs );
    return ERROR_SUCCESS;
}

/* caller must hold counter lock */
static DWORD counter_item_count( const struct counter *counter )
{
    return is_wildcard_source( counter ) ? counter->instance_count : 1;
}

/* caller must hold counter lock; name is the instance part of a non-wildcard path */
static void counter_item( struct counter *counter, DWORD i, const WCHAR **name, DWORD *name_len,
                          PDH_RAW_COUNTER **cur, PDH_RAW_COUNTER **prev )
{
    if (is_wildcard_source( counter ))
    {
        *name = counter->instances[i].name;
        *name_len = wcslen( *name );
        *cur = &counter->instances[i].raw[1];
        *prev = &counter->instances[i].raw[0];
    }
    else
    {
        const WCHAR *start = wcschr( counter->path, '(' ), *end = start ? wcschr( start, ')' ) : NULL;

        *name = start && end ? start + 1 : L"";
        *name_len = start && end ? end - start - 1 : 0;
        *cur = &counter->raw[1];
        *prev = &counter->raw[0];
    }
}

static PDH_STATUS get_counter_array( PDH_HCOUNTER handle, BOOL formatted, DWORD format, BOOL ansi,
                                     DWORD *size, DWORD *count, void *buffer )
{
    struct counter *counter = handle;
    DWORD i, n, item_size, required, name_len;
    PDH_RAW_COUNTER *cur, *prev;
    const WCHAR *name;
    char *strings;

    if (!size || !count) return PDH_INVALID_ARGUMENT;

    EnterCriticalSection( &pdh_handle_cs );
    if (!counter || counter->magic != PDH_MAGIC_COUNTER)
    {
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_INVALID_HANDLE;
    }
    if (!uses_raw_samples( counter ))
    {
        LeaveCriticalSection( &pdh_handle_cs );
        FIXME( "array of %s not supported\n", debugstr_w(counter->path) );
        return PDH_NOT_IMPLEMENTED;
    }

    item_size = formatted ? (ansi ? sizeof(PDH_FMT_COUNTERVALUE_ITEM_A) : sizeof(PDH_FMT_COUNTERVALUE_ITEM_W))
                          : (ansi ? sizeof(PDH_RAW_COUNTER_ITEM_A) : sizeof(PDH_RAW_COUNTER_ITEM_W));
    n = counter_item_count( counter );
    required = n * item_size;
    for (i = 0; i < n; i++)
    {
        counter_item( counter, i, &name, &name_len, &cur, &prev );
        required += ansi ? WideCharToMultiByte( CP_ACP, 0, name, name_len, NULL, 0, NULL, NULL ) + 1
                         : (name_len + 1) * sizeof(WCHAR);
    }

    *count = n;
    if (!buffer || *size < required)
    {
        *size = required;
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_MORE_DATA;
    }
    *size = required;

    strings = (char *)buffer + n * item_size;
    for (i = 0; i < n; i++)
    {
        char *item = (char *)buffer + i * item_size;
        char *str = strings;

        counter_item( counter, i, &name, &name_len, &cur, &prev );
        if (ansi)
        {
            int len = WideCharToMultiByte( CP_ACP, 0, name, name_len, str, required, NULL, NULL );
            str[len] = 0;
            strings += len + 1;
        }
        else
        {
            memcpy( str, name, name_len * sizeof(WCHAR) );
            ((WCHAR *)str)[name_len] = 0;
            strings += (name_len + 1) * sizeof(WCHAR);
        }

        if (formatted)
        {
            PDH_FMT_COUNTERVALUE *value = ansi ? &((PDH_FMT_COUNTERVALUE_ITEM_A *)item)->FmtValue
                                               : &((PDH_FMT_COUNTERVALUE_ITEM_W *)item)->FmtValue;
            PDH_STATUS status;

            memset( value, 0, sizeof(*value) );
            status = calculate_raw( counter->type, format, counter->scale ? counter->scale : counter->defaultscale,
                                    cur, prev, value );
            if (status) value->CStatus = status;
            if (ansi) ((PDH_FMT_COUNTERVALUE_ITEM_A *)item)->szName = str;
            else ((PDH_FMT_COUNTERVALUE_ITEM_W *)item)->szName = (WCHAR *)str;
        }
        else if (ansi)
        {
            ((PDH_RAW_COUNTER_ITEM_A *)item)->szName = str;
            ((PDH_RAW_COUNTER_ITEM_A *)item)->RawValue = *cur;
        }
        else
        {
            ((PDH_RAW_COUNTER_ITEM_W *)item)->szName = (WCHAR *)str;
            ((PDH_RAW_COUNTER_ITEM_W *)item)->RawValue = *cur;
        }
    }

    LeaveCriticalSection( &pdh_handle_cs );
    return ERROR_SUCCESS;
}

/***********************************************************************
 *              PdhGetFormattedCounterArrayW   (PDH.@)
 */
PDH_STATUS WINAPI PdhGetFormattedCounterArrayW( PDH_HCOUNTER handle, DWORD format, DWORD *size, DWORD *count,
                                                PDH_FMT_COUNTERVALUE_ITEM_W *buffer )
{
    TRACE( "%p %lx %p %p %p\n", handle, format, size, count, buffer );
    return get_counter_array( handle, TRUE, format, FALSE, size, count, buffer );
}

/***********************************************************************
 *              PdhGetFormattedCounterArrayA   (PDH.@)
 */
PDH_STATUS WINAPI PdhGetFormattedCounterArrayA( PDH_HCOUNTER handle, DWORD format, DWORD *size, DWORD *count,
                                                PDH_FMT_COUNTERVALUE_ITEM_A *buffer )
{
    TRACE( "%p %lx %p %p %p\n", handle, format, size, count, buffer );
    return get_counter_array( handle, TRUE, format, TRUE, size, count, buffer );
}

/***********************************************************************
 *              PdhFormatFromRawValue   (PDH.@)
 */
PDH_STATUS WINAPI PdhFormatFromRawValue( DWORD type, DWORD format, LONGLONG *timebase,
                                         PDH_RAW_COUNTER *raw1, PDH_RAW_COUNTER *raw2, PDH_FMT_COUNTERVALUE *value )
{
    TRACE( "%#lx %#lx %p %p %p %p\n", type, format, timebase, raw1, raw2, value );

    if (!value) return PDH_INVALID_ARGUMENT;
    return calculate_raw( type, format, 0, raw1, raw2, value );
}

/***********************************************************************
 *              PdhLookupPerfIndexByNameA   (PDH.@)
 */
PDH_STATUS WINAPI PdhLookupPerfIndexByNameA( LPCSTR machine, LPCSTR name, LPDWORD index )
{
    PDH_STATUS ret;
    WCHAR *machineW = NULL;
    WCHAR *nameW;

    TRACE("%s %s %p\n", debugstr_a(machine), debugstr_a(name), index);

    if (!name) return PDH_INVALID_ARGUMENT;

    if (machine && !(machineW = pdh_strdup_aw( machine ))) return PDH_MEMORY_ALLOCATION_FAILURE;

    if (!(nameW = pdh_strdup_aw( name )))
        return PDH_MEMORY_ALLOCATION_FAILURE;

    ret = PdhLookupPerfIndexByNameW( machineW, nameW, index );

    free( nameW );
    free( machineW );
    return ret;
}

/***********************************************************************
 *              PdhLookupPerfIndexByNameW   (PDH.@)
 */
PDH_STATUS WINAPI PdhLookupPerfIndexByNameW( LPCWSTR machine, LPCWSTR name, LPDWORD index )
{
    unsigned int i;

    TRACE("%s %s %p\n", debugstr_w(machine), debugstr_w(name), index);

    if (!name || !index) return PDH_INVALID_ARGUMENT;

    if (machine)
    {
        FIXME("remote machine not supported\n");
        return PDH_CSTATUS_NO_MACHINE;
    }
    for (i = 0; i < ARRAY_SIZE(counter_sources); i++)
    {
        if (pdh_match_path( counter_sources[i].path, name ))
        {
            *index = counter_sources[i].index;
            return ERROR_SUCCESS;
        }
    }
    return PDH_STRING_NOT_FOUND;
}

/***********************************************************************
 *              PdhLookupPerfNameByIndexA   (PDH.@)
 */
PDH_STATUS WINAPI PdhLookupPerfNameByIndexA( LPCSTR machine, DWORD index, LPSTR buffer, LPDWORD size )
{
    PDH_STATUS ret;
    WCHAR *machineW = NULL;
    WCHAR bufferW[PDH_MAX_COUNTER_NAME];
    DWORD sizeW = ARRAY_SIZE(bufferW);

    TRACE("%s %ld %p %p\n", debugstr_a(machine), index, buffer, size);

    if (!buffer || !size) return PDH_INVALID_ARGUMENT;

    if (machine && !(machineW = pdh_strdup_aw( machine ))) return PDH_MEMORY_ALLOCATION_FAILURE;

    if (!(ret = PdhLookupPerfNameByIndexW( machineW, index, bufferW, &sizeW )))
    {
        int required = WideCharToMultiByte( CP_ACP, 0, bufferW, -1, NULL, 0, NULL, NULL );

        if (*size < required) ret = PDH_MORE_DATA;
        else WideCharToMultiByte( CP_ACP, 0, bufferW, -1, buffer, required, NULL, NULL );
        *size = required;
    }
    free( machineW );
    return ret;
}

/***********************************************************************
 *              PdhLookupPerfNameByIndexW   (PDH.@)
 */
PDH_STATUS WINAPI PdhLookupPerfNameByIndexW( LPCWSTR machine, DWORD index, LPWSTR buffer, LPDWORD size )
{
    PDH_STATUS ret;
    unsigned int i;

    TRACE("%s %ld %p %p\n", debugstr_w(machine), index, buffer, size);

    if (machine)
    {
        FIXME("remote machine not supported\n");
        return PDH_CSTATUS_NO_MACHINE;
    }

    if (!buffer || !size) return PDH_INVALID_ARGUMENT;
    if (!index) return ERROR_SUCCESS;

    for (i = 0; i < ARRAY_SIZE(counter_sources); i++)
    {
        if (counter_sources[i].index == index)
        {
            WCHAR *p = wcsrchr( counter_sources[i].path, '\\' ) + 1;
            unsigned int required = lstrlenW( p ) + 1;

            if (*size < required) ret = PDH_MORE_DATA;
            else
            {
                lstrcpyW( buffer, p );
                ret = ERROR_SUCCESS;
            }
            *size = required;
            return ret;
        }
    }
    return PDH_INVALID_ARGUMENT;
}

/***********************************************************************
 *              PdhOpenQueryA   (PDH.@)
 */
PDH_STATUS WINAPI PdhOpenQueryA( LPCSTR source, DWORD_PTR userdata, PDH_HQUERY *query )
{
    PDH_STATUS ret;
    WCHAR *sourceW = NULL;

    TRACE("%s %Ix %p\n", debugstr_a(source), userdata, query);

    if (source && !(sourceW = pdh_strdup_aw( source ))) return PDH_MEMORY_ALLOCATION_FAILURE;

    ret = PdhOpenQueryW( sourceW, userdata, query );
    free( sourceW );

    return ret;
}

/***********************************************************************
 *              PdhOpenQueryW   (PDH.@)
 */
PDH_STATUS WINAPI PdhOpenQueryW( LPCWSTR source, DWORD_PTR userdata, PDH_HQUERY *handle )
{
    struct query *query;

    TRACE("%s %Ix %p\n", debugstr_w(source), userdata, handle);

    if (!handle) return PDH_INVALID_ARGUMENT;

    if (source)
    {
        FIXME("log file data source not supported\n");
        return PDH_INVALID_ARGUMENT;
    }
    if ((query = create_query()))
    {
        query->user = userdata;
        *handle = query;

        return ERROR_SUCCESS;
    }
    return PDH_MEMORY_ALLOCATION_FAILURE;
}

/***********************************************************************
 *              PdhRemoveCounter   (PDH.@)
 */
PDH_STATUS WINAPI PdhRemoveCounter( PDH_HCOUNTER handle )
{
    struct counter *counter = handle;

    TRACE("%p\n", handle);

    EnterCriticalSection( &pdh_handle_cs );
    if (!counter || counter->magic != PDH_MAGIC_COUNTER)
    {
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_INVALID_HANDLE;
    }

    list_remove( &counter->entry );
    destroy_counter( counter );

    LeaveCriticalSection( &pdh_handle_cs );
    return ERROR_SUCCESS;
}

/***********************************************************************
 *              PdhSetCounterScaleFactor   (PDH.@)
 */
PDH_STATUS WINAPI PdhSetCounterScaleFactor( PDH_HCOUNTER handle, LONG factor )
{
    struct counter *counter = handle;

    TRACE("%p\n", handle);

    EnterCriticalSection( &pdh_handle_cs );
    if (!counter || counter->magic != PDH_MAGIC_COUNTER)
    {
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_INVALID_HANDLE;
    }
    if (factor < PDH_MIN_SCALE || factor > PDH_MAX_SCALE)
    {
        LeaveCriticalSection( &pdh_handle_cs );
        return PDH_INVALID_ARGUMENT;
    }

    counter->scale = factor;

    LeaveCriticalSection( &pdh_handle_cs );
    return ERROR_SUCCESS;
}

/***********************************************************************
 *              PdhValidatePathA   (PDH.@)
 */
PDH_STATUS WINAPI PdhValidatePathA( LPCSTR path )
{
    PDH_STATUS ret;
    WCHAR *pathW;

    TRACE("%s\n", debugstr_a(path));

    if (!path) return PDH_INVALID_ARGUMENT;
    if (!(pathW = pdh_strdup_aw( path ))) return PDH_MEMORY_ALLOCATION_FAILURE;

    ret = PdhValidatePathW( pathW );

    free( pathW );
    return ret;
}

static PDH_STATUS validate_path( LPCWSTR path )
{
    if (!path || !*path) return PDH_INVALID_ARGUMENT;
    if (*path++ != '\\' || !wcschr( path, '\\' )) return PDH_CSTATUS_BAD_COUNTERNAME;
    return ERROR_SUCCESS;
 }

/***********************************************************************
 *              PdhValidatePathW   (PDH.@)
 */
PDH_STATUS WINAPI PdhValidatePathW( LPCWSTR path )
{
    PDH_STATUS ret;
    unsigned int i;

    TRACE("%s\n", debugstr_w(path));

    if ((ret = validate_path( path ))) return ret;

    for (i = 0; i < ARRAY_SIZE(counter_sources); i++)
        if (pdh_match_path( counter_sources[i].path, path )) return ERROR_SUCCESS;

    return PDH_CSTATUS_NO_COUNTER;
}

/***********************************************************************
 *              PdhVbAddCounter   (PDH.@)
 */
PDH_STATUS WINAPI PdhVbAddCounter( PDH_HQUERY query, LPCSTR path, PDH_HCOUNTER *counter )
{
    FIXME("%p, %s, %p: stub!\n", query, debugstr_a(path), counter);

    if (!path) return PDH_INVALID_ARGUMENT;

    return PDH_NOT_IMPLEMENTED;
}

/***********************************************************************
 *              PdhVbGetDoubleCounterValue   (PDH.@)
 */
double WINAPI PdhVbGetDoubleCounterValue( PDH_HCOUNTER handle, PDH_STATUS *counter_status )
{
    PDH_FMT_COUNTERVALUE value;
    PDH_STATUS status;

    TRACE( "%p %p\n", handle, counter_status );

    memset( &value, 0, sizeof(value) );
    status = PdhGetFormattedCounterValue( handle, PDH_FMT_DOUBLE, NULL, &value );

    if (counter_status) *counter_status = status;
    return value.doubleValue;
}

/***********************************************************************
 *              PdhValidatePathExA   (PDH.@)
 */
PDH_STATUS WINAPI PdhValidatePathExA( PDH_HLOG source, LPCSTR path )
{
    TRACE("%p %s\n", source, debugstr_a(path));

    if (source)
    {
        FIXME("log file data source not supported\n");
        return ERROR_SUCCESS;
    }
    return PdhValidatePathA( path );
}

/***********************************************************************
 *              PdhValidatePathExW   (PDH.@)
 */
PDH_STATUS WINAPI PdhValidatePathExW( PDH_HLOG source, LPCWSTR path )
{
    TRACE("%p %s\n", source, debugstr_w(path));

    if (source)
    {
        FIXME("log file data source not supported\n");
        return ERROR_SUCCESS;
    }
    return PdhValidatePathW( path );
}

/***********************************************************************
 *              PdhMakeCounterPathA   (PDH.@)
 */
PDH_STATUS WINAPI PdhMakeCounterPathA( PDH_COUNTER_PATH_ELEMENTS_A *e, LPSTR buffer,
                                       LPDWORD buflen, DWORD flags )
{
    PDH_STATUS ret = PDH_MEMORY_ALLOCATION_FAILURE;
    PDH_COUNTER_PATH_ELEMENTS_W eW;
    WCHAR *bufferW;
    DWORD buflenW;

    TRACE("%p %p %p 0x%08lx\n", e, buffer, buflen, flags);

    if (!e || !buflen) return PDH_INVALID_ARGUMENT;

    memset( &eW, 0, sizeof(eW) );
    if (e->szMachineName    && !(eW.szMachineName    = pdh_strdup_aw( e->szMachineName ))) goto done;
    if (e->szObjectName     && !(eW.szObjectName     = pdh_strdup_aw( e->szObjectName ))) goto done;
    if (e->szInstanceName   && !(eW.szInstanceName   = pdh_strdup_aw( e->szInstanceName ))) goto done;
    if (e->szParentInstance && !(eW.szParentInstance = pdh_strdup_aw( e->szParentInstance ))) goto done;
    if (e->szCounterName    && !(eW.szCounterName    = pdh_strdup_aw( e->szCounterName ))) goto done;
    eW.dwInstanceIndex = e->dwInstanceIndex;

    buflenW = 0;
    ret = PdhMakeCounterPathW( &eW, NULL, &buflenW, flags );
    if (ret == PDH_MORE_DATA)
    {
        if ((bufferW = malloc( buflenW * sizeof(WCHAR) )))
        {
            if (!(ret = PdhMakeCounterPathW( &eW, bufferW, &buflenW, flags )))
            {
                int len = WideCharToMultiByte(CP_ACP, 0, bufferW, -1, NULL, 0, NULL, NULL);
                if (*buflen >= len) WideCharToMultiByte(CP_ACP, 0, bufferW, -1, buffer, *buflen, NULL, NULL);
                else ret = PDH_MORE_DATA;
                *buflen = len;
            }
            free( bufferW );
        }
        else
            ret = PDH_MEMORY_ALLOCATION_FAILURE;
    }

done:
    free( eW.szMachineName );
    free( eW.szObjectName );
    free( eW.szInstanceName );
    free( eW.szParentInstance );
    free( eW.szCounterName );
    return ret;
}

/***********************************************************************
 *              PdhMakeCounterPathW   (PDH.@)
 */
PDH_STATUS WINAPI PdhMakeCounterPathW( PDH_COUNTER_PATH_ELEMENTS_W *e, LPWSTR buffer,
                                       LPDWORD buflen, DWORD flags )
{
    WCHAR path[PDH_MAX_COUNTER_NAME], instance[12];
    PDH_STATUS ret = ERROR_SUCCESS;
    DWORD len;

    TRACE("%p %p %p 0x%08lx\n", e, buffer, buflen, flags);

    if (flags) FIXME("unimplemented flags 0x%08lx\n", flags);

    if (!e || !e->szCounterName || !e->szObjectName || !buflen)
        return PDH_INVALID_ARGUMENT;

    path[0] = 0;
    if (e->szMachineName)
    {
        lstrcatW(path, L"\\\\");
        lstrcatW(path, e->szMachineName);
    }
    lstrcatW(path, L"\\");
    lstrcatW(path, e->szObjectName);
    if (e->szInstanceName)
    {
        lstrcatW(path, L"(");
        if (e->szParentInstance)
        {
            lstrcatW(path, e->szParentInstance);
            lstrcatW(path, L"/");
        }
        lstrcatW(path, e->szInstanceName);
        swprintf(instance, ARRAY_SIZE(instance), L"#%u", e->dwInstanceIndex);
        lstrcatW(path, instance);
        lstrcatW(path, L")");
    }
    lstrcatW(path, L"\\");
    lstrcatW(path, e->szCounterName);

    len = lstrlenW(path) + 1;
    if (*buflen >= len) lstrcpyW(buffer, path);
    else ret = PDH_MORE_DATA;
    *buflen = len;
    return ret;
}

/***********************************************************************
 *              PdhEnumObjectItemsA   (PDH.@)
 */
PDH_STATUS WINAPI PdhEnumObjectItemsA(LPCSTR szDataSource, LPCSTR szMachineName, LPCSTR szObjectName,
                                      LPSTR mszCounterList, LPDWORD pcchCounterListLength, LPSTR mszInstanceList,
                                      LPDWORD pcchInstanceListLength, DWORD dwDetailLevel, DWORD dwFlags)
{
    FIXME("%s, %s, %s, %p, %p, %p, %p, %ld, 0x%lx: stub\n", debugstr_a(szDataSource), debugstr_a(szMachineName),
         debugstr_a(szObjectName), mszCounterList, pcchCounterListLength, mszInstanceList,
         pcchInstanceListLength, dwDetailLevel, dwFlags);

    return PDH_NOT_IMPLEMENTED;
}

/***********************************************************************
 *              PdhEnumObjectItemsW   (PDH.@)
 */
PDH_STATUS WINAPI PdhEnumObjectItemsW(LPCWSTR szDataSource, LPCWSTR szMachineName, LPCWSTR szObjectName,
                                      LPWSTR mszCounterList, LPDWORD pcchCounterListLength, LPWSTR mszInstanceList,
                                      LPDWORD pcchInstanceListLength, DWORD dwDetailLevel, DWORD dwFlags)
{
    FIXME("%s, %s, %s, %p, %p, %p, %p, %ld, 0x%lx: stub\n", debugstr_w(szDataSource), debugstr_w(szMachineName),
         debugstr_w(szObjectName), mszCounterList, pcchCounterListLength, mszInstanceList,
         pcchInstanceListLength, dwDetailLevel, dwFlags);

    return PDH_NOT_IMPLEMENTED;
}

/***********************************************************************
 *              PdhSetDefaultRealTimeDataSource   (PDH.@)
 */
PDH_STATUS WINAPI PdhSetDefaultRealTimeDataSource( DWORD source )
{
    FIXME("%lu\n", source);
    return ERROR_SUCCESS;
}

/***********************************************************************
 *              PdhGetLogFileTypeA   (PDH.@)
 */
PDH_STATUS WINAPI PdhGetLogFileTypeA(const char *log, DWORD *type)
{
    FIXME("%s, %p: stub\n", debugstr_a(log), type);
    return PDH_NOT_IMPLEMENTED;
}

/***********************************************************************
 *              PdhGetLogFileTypeW   (PDH.@)
 */
PDH_STATUS WINAPI PdhGetLogFileTypeW(const WCHAR *log, DWORD *type)
{
    FIXME("%s, %p: stub\n", debugstr_w(log), type);
    return PDH_NOT_IMPLEMENTED;
}

/***********************************************************************
 *              PdhBindInputDataSourceA   (PDH.@)
 */
PDH_STATUS WINAPI PdhBindInputDataSourceA(PDH_HLOG *source, const char *filenamelist)
{
    FIXME("%p %s: stub\n", source, debugstr_a(filenamelist));
    return PDH_NOT_IMPLEMENTED;
}

/***********************************************************************
 *              PdhBindInputDataSourceW   (PDH.@)
 */
PDH_STATUS WINAPI PdhBindInputDataSourceW(PDH_HLOG *source, const WCHAR *filenamelist)
{
    FIXME("%p %s: stub\n", source, debugstr_w(filenamelist));
    return PDH_NOT_IMPLEMENTED;
}

/***********************************************************************
 *              PdhConnectMachineA   (PDH.@)
 */
PDH_STATUS WINAPI PdhConnectMachineA(const char *name)
{
    FIXME("%s: stub\n", debugstr_a(name));
    return PDH_NOT_IMPLEMENTED;
}

/***********************************************************************
 *              PdhEnumObjectsA   (PDH.@)
 */
PDH_STATUS WINAPI PdhEnumObjectsA(const char *source, const char *machine, char *list,
                                  DWORD *size, DWORD detail, BOOL refresh)
{
    FIXME("%s %s %p %p %lu %d: stub\n", debugstr_a(source),debugstr_a(machine), list, size, detail, refresh);
    return PDH_NOT_IMPLEMENTED;
}

/***********************************************************************
 *              PdhEnumObjectsW   (PDH.@)
 */
PDH_STATUS WINAPI PdhEnumObjectsW(const WCHAR *source, const WCHAR *machine, WCHAR *list,
                                  DWORD *size, DWORD detail, BOOL refresh)
{
    FIXME("%s %s %p %p %lu %d: stub\n", debugstr_w(source),debugstr_w(machine), list, size, detail, refresh);
    return PDH_NOT_IMPLEMENTED;
}

/***********************************************************************
 *              PdhGetRawCounterArrayA   (PDH.@)
 */
PDH_STATUS WINAPI PdhGetRawCounterArrayA( PDH_HCOUNTER counter, DWORD *size, DWORD *count, PDH_RAW_COUNTER_ITEM_A *buffer )
{
    TRACE( "%p %p %p %p\n", counter, size, count, buffer );
    return get_counter_array( counter, FALSE, 0, TRUE, size, count, buffer );
}

/***********************************************************************
 *              PdhGetRawCounterArrayW   (PDH.@)
 */
PDH_STATUS WINAPI PdhGetRawCounterArrayW( PDH_HCOUNTER counter, DWORD *size, DWORD *count, PDH_RAW_COUNTER_ITEM_W *buffer )
{
    TRACE( "%p %p %p %p\n", counter, size, count, buffer );
    return get_counter_array( counter, FALSE, 0, FALSE, size, count, buffer );
}
