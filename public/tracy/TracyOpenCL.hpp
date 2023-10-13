#ifndef __TRACYOPENCL_HPP__
#define __TRACYOPENCL_HPP__

#if !defined TRACY_ENABLE

#define TracyCLContext(c, x) nullptr
#define TracyCLDestroy(c)
#define TracyCLContextName(c, x, y)

#define TracyCLNamedZone(c, x, y, z, w)
#define TracyCLNamedZoneC(c, x, y, z, w, v)
#define TracyCLZone(c, x, y)
#define TracyCLZoneC(c, x, y)
#define TracyCLZoneTransient(c,x,y,z)

#define TracyCLNamedZoneS(c, x, y, z, w, v)
#define TracyCLNamedZoneCS(c, x, y, z, w, v)
#define TracyCLZoneS(c, x, y)
#define TracyCLZoneCS(c, x, y, z)
#define TracyCLZoneTransientS(c,x,y,z,w)

#define TracyCLNamedZoneSetEvent(x, e)
#define TracyCLZoneSetEvent(e)

#define TracyCLCollect(c)

namespace tracy
{
    class OpenCLCtxScope {};
}

using TracyCLCtx = void*;

#else

#include <atomic>
#include <cassert>
#include <sstream>
#include <fstream>

#include "Tracy.hpp"
#include "../client/TracyCallstack.hpp"
#include "../client/TracyProfiler.hpp"
#include "../common/TracyAlloc.hpp"

#define TRACY_CL_TO_STRING_INDIRECT(T) #T
#define TRACY_CL_TO_STRING(T) TRACY_CL_TO_STRING_INDIRECT(T)
#define TRACY_CL_ASSERT(p) if(!(p)) {                                                         \
    TracyMessageL( "TRACY_CL_ASSERT failed on " TracyFile ":" TRACY_CL_TO_STRING(TracyLine) );  \
    assert(false && "TRACY_CL_ASSERT failed");                                                \
}
#define TRACY_CL_CHECK_ERROR(err) if(err != CL_SUCCESS) {                    \
    std::ostringstream oss;                                                  \
    oss << "TRACY_CL_CHECK_ERROR failed on " << TracyFile << ":" << TracyLine  \
        << ": error code " << err;                                           \
    auto msg = oss.str();                                                    \
    TracyMessage(msg.data(), msg.size());                                    \
    assert(false && "TRACY_CL_CHECK_ERROR failed");                          \
}

namespace tracy {

    enum class EventPhase : uint8_t
    {
        Begin,
        End
    };

    inline int64_t m_tcpu = 0;

    static inline void enable_set_cpu_time()
    {
        ZoneScoped;
        if ( m_tcpu == 0)
        {
            m_tcpu = 1;
        }
    }

    static inline void set_cpu_time()
    {
        ZoneScoped;
        if ( m_tcpu == 1)
        {
            m_tcpu = Profiler::GetTime();
        }
    }

    struct TTDeviceEvent
    {
        uint16_t chip;
        uint16_t core_x;
        uint16_t core_y;
        uint16_t risc;
        uint16_t marker;

        TTDeviceEvent (): chip(-1),core_x(-1),core_y(-1),risc(-1),marker(-1)
        {
        }

        TTDeviceEvent (
                uint16_t chip,
                uint16_t core_x,
                uint16_t core_y,
                uint16_t risc,
                uint16_t marker
                ): chip(chip),core_x(core_x),core_y(core_y),risc(risc),marker(marker)
        {
        }
    };

    struct EventInfo
    {
        TTDeviceEvent event;
        EventPhase phase;
    };

    class OpenCLCtx
    {
    public:
        enum { QueryCount = 64 * 1024 };

        OpenCLCtx()
            : m_contextId(GetGpuCtxCounter().fetch_add(1, std::memory_order_relaxed))
            , m_head(0)
            , m_tail(0)
        {
            ZoneScopedC(Color::Red4);


        }

        void PopulateCLContext()
        {
            ZoneScopedC(Color::Red);
            static bool done = false;
            if (!done)
            {
                int64_t tcpu, tgpu;
                tcpu = m_tcpu;
                tgpu = m_tcpu;

                auto item = Profiler::QueueSerial();
                MemWrite(&item->hdr.type, QueueType::GpuNewContext);
                MemWrite(&item->gpuNewContext.cpuTime, tcpu);
                MemWrite(&item->gpuNewContext.gpuTime, tgpu);
                memset(&item->gpuNewContext.thread, 0, sizeof(item->gpuNewContext.thread));
                MemWrite(&item->gpuNewContext.period, 1.0f);
                MemWrite(&item->gpuNewContext.type, GpuContextType::tt_device);
                MemWrite(&item->gpuNewContext.context, (uint8_t) m_contextId);
                MemWrite(&item->gpuNewContext.flags, (uint8_t)0);
#ifdef TRACY_ON_DEMAND
                GetProfiler().DeferItem(*item);
#endif
                Profiler::QueueSerialFinish();
                done = true;
            }
        }

        void Name( const char* name, uint16_t len )
        {
            auto ptr = (char*)tracy_malloc( len );
            memcpy( ptr, name, len );

            auto item = Profiler::QueueSerial();
            MemWrite( &item->hdr.type, QueueType::GpuContextName );
            MemWrite( &item->gpuContextNameFat.context, (uint8_t)m_contextId );
            MemWrite( &item->gpuContextNameFat.ptr, (uint64_t)ptr );
            MemWrite( &item->gpuContextNameFat.size, len );
#ifdef TRACY_ON_DEMAND
            GetProfiler().DeferItem( *item );
#endif
            Profiler::QueueSerialFinish();
        }

        void Collect(std::unordered_map<uint64_t,std::list<uint64_t>>& device_data)
        {
            ZoneScopedC(Color::Red4);

            if (m_tail == m_head) return;

#ifdef TRACY_ON_DEMAND
            if (!GetProfiler().IsConnected())
            {
                m_head = m_tail = 0;
            }
#endif

            static uint64_t shift = 0;

            for (auto& data: device_data)
            {
                //std::cout << "ID: "<<  data.first << std::endl;
                for (auto timestamp: data.second)
                {

                    //std::cout << "    TS: "<<  timestamp << std::endl;
                    if ((shift == 0) || (shift > timestamp))
                    {
                        shift = timestamp;
                    }
                }
            }

            for (; m_tail != m_head; m_tail = (m_tail + 1) % QueryCount)
            {

                ZoneScopedNC("Add Marker", Color::Red4);
                EventInfo eventInfo = GetQuery(m_tail);

                //std::cout << "t,h"<<  m_tail << m_head << std::endl;
                uint64_t threadID = eventInfo.event.core_x*1000000+eventInfo.event.core_y*10000+eventInfo.event.risc*100; 
                uint64_t eventID;

                if (eventInfo.phase == EventPhase::Begin)
                {
                    if (eventInfo.event.marker == 1)
                    {
                        eventID = 1;
                    }
                    else
                    {
                        eventID = 2;
                    }
                }
                else
                {
                    if (eventInfo.event.marker == 1)
                    {
                        eventID = 4;
                    }
                    else
                    {
                        eventID = 3;
                    }
                }
                
                eventID = eventID + threadID;
                
                if (device_data.find (eventID) != device_data.end() && device_data.at(eventID).size() > 0)
                {
                    //std::cout << "data found: " << eventID << std::endl;
                    uint64_t rawTime = device_data.at(eventID).front();
                    device_data.at(eventID).pop_front();
                    uint64_t timestamp = (rawTime - shift)*1000;
                    timestamp = timestamp/(1202);

                    auto item = Profiler::QueueSerial();
                    MemWrite(&item->hdr.type, QueueType::GpuTime);
                    MemWrite(&item->gpuTime.gpuTime, (int64_t)(timestamp + m_tcpu));
                    MemWrite(&item->gpuTime.queryId, (uint16_t)m_tail);
                    MemWrite(&item->gpuTime.context, m_contextId);
                    Profiler::QueueSerialFinish();
                }

            }
        }

        tracy_force_inline uint8_t GetId() const
        {
            return m_contextId;
        }

        tracy_force_inline unsigned int NextQueryId(EventInfo eventInfo)
        {
            const auto id = m_head;
            m_head = (m_head + 1) % QueryCount;
            TRACY_CL_ASSERT(m_head != m_tail);
            m_query[id] = eventInfo;
            return id;
        }

        tracy_force_inline EventInfo& GetQuery(unsigned int id)
        {
            TRACY_CL_ASSERT(id < QueryCount);
            return m_query[id];
        }

    private:

        unsigned int m_contextId;

        EventInfo m_query[QueryCount];
        unsigned int m_head; // index at which a new event should be inserted
        unsigned int m_tail; // oldest event

    };

    class OpenCLCtxScope {
    public:
        uint32_t m_threadID;
        tracy_force_inline OpenCLCtxScope(OpenCLCtx* ctx, const SourceLocationData* srcLoc, bool is_active, int threadID)
#ifdef TRACY_ON_DEMAND
            : m_active(is_active&& GetProfiler().IsConnected())
#else
            : m_active(is_active)
#endif
            , m_ctx(ctx)
            , m_event(TTDeviceEvent ())
            , m_threadID (threadID)
        {
            if (!m_active) return;

            m_beginQueryId = ctx->NextQueryId(EventInfo{ TTDeviceEvent (), EventPhase::Begin });

            auto item = Profiler::QueueSerial();
            MemWrite(&item->hdr.type, QueueType::GpuZoneBeginSerial);
            MemWrite(&item->gpuZoneBegin.cpuTime, Profiler::GetTime());
            MemWrite(&item->gpuZoneBegin.srcloc, (uint64_t)srcLoc);
            MemWrite(&item->gpuZoneBegin.thread, (uint32_t)m_threadID);
            MemWrite(&item->gpuZoneBegin.queryId, (uint16_t)m_beginQueryId);
            MemWrite(&item->gpuZoneBegin.context, ctx->GetId());
            Profiler::QueueSerialFinish();
        }

        tracy_force_inline OpenCLCtxScope(OpenCLCtx* ctx, const SourceLocationData* srcLoc, int depth, bool is_active)
#ifdef TRACY_ON_DEMAND
            : m_active(is_active&& GetProfiler().IsConnected())
#else
            : m_active(is_active)
#endif
            , m_ctx(ctx)
            , m_event(TTDeviceEvent ())
        {
            if (!m_active) return;

            m_beginQueryId = ctx->NextQueryId(EventInfo{ TTDeviceEvent (), EventPhase::Begin });

            GetProfiler().SendCallstack(depth);

            auto item = Profiler::QueueSerial();
            MemWrite(&item->hdr.type, QueueType::GpuZoneBeginCallstackSerial);
            MemWrite(&item->gpuZoneBegin.cpuTime, Profiler::GetTime());
            MemWrite(&item->gpuZoneBegin.srcloc, (uint64_t)srcLoc);
            MemWrite(&item->gpuZoneBegin.thread, 56);
            MemWrite(&item->gpuZoneBegin.queryId, (uint16_t)m_beginQueryId);
            MemWrite(&item->gpuZoneBegin.context, ctx->GetId());
            Profiler::QueueSerialFinish();
        }

        tracy_force_inline OpenCLCtxScope(OpenCLCtx* ctx, uint32_t line, const char* source, size_t sourceSz, const char* function, size_t functionSz, const char* name, size_t nameSz, bool is_active)
#ifdef TRACY_ON_DEMAND
            : m_active(is_active && GetProfiler().IsConnected())
#else
            : m_active(is_active)
#endif
            , m_ctx(ctx)
            , m_event(TTDeviceEvent ())
        {
            if (!m_active) return;

            m_beginQueryId = ctx->NextQueryId(EventInfo{ TTDeviceEvent (), EventPhase::Begin });

            const auto srcloc = Profiler::AllocSourceLocation( line, source, sourceSz, function, functionSz, name, nameSz );
            auto item = Profiler::QueueSerial();
            MemWrite( &item->hdr.type, QueueType::GpuZoneBeginAllocSrcLocSerial );
            MemWrite(&item->gpuZoneBegin.cpuTime, Profiler::GetTime());
            MemWrite(&item->gpuZoneBegin.srcloc, srcloc);
            MemWrite(&item->gpuZoneBegin.thread, 56);
            MemWrite(&item->gpuZoneBegin.queryId, (uint16_t)m_beginQueryId);
            MemWrite(&item->gpuZoneBegin.context, ctx->GetId());
            Profiler::QueueSerialFinish();
        }

        tracy_force_inline OpenCLCtxScope(OpenCLCtx* ctx, uint32_t line, const char* source, size_t sourceSz, const char* function, size_t functionSz, const char* name, size_t nameSz, int depth, bool is_active)
#ifdef TRACY_ON_DEMAND
            : m_active(is_active && GetProfiler().IsConnected())
#else
            : m_active(is_active)
#endif
            , m_ctx(ctx)
            , m_event(TTDeviceEvent ())
        {
            if (!m_active) return;

            m_beginQueryId = ctx->NextQueryId(EventInfo{ TTDeviceEvent (), EventPhase::Begin });

            const auto srcloc = Profiler::AllocSourceLocation( line, source, sourceSz, function, functionSz, name, nameSz );
            auto item = Profiler::QueueSerialCallstack( Callstack( depth ) );
            MemWrite(&item->hdr.type, QueueType::GpuZoneBeginAllocSrcLocCallstackSerial);
            MemWrite(&item->gpuZoneBegin.cpuTime, Profiler::GetTime());
            MemWrite(&item->gpuZoneBegin.srcloc, srcloc);
            MemWrite(&item->gpuZoneBegin.thread, 56);
            MemWrite(&item->gpuZoneBegin.queryId, (uint16_t)m_beginQueryId);
            MemWrite(&item->gpuZoneBegin.context, ctx->GetId());
            Profiler::QueueSerialFinish();
        }

        tracy_force_inline void SetEvent(TTDeviceEvent event)
        {
            if (!m_active) return;
            m_event = event;
            //TRACY_CL_CHECK_ERROR(clRetainEvent(m_event));
            m_ctx->GetQuery(m_beginQueryId).event = m_event;
        }

        tracy_force_inline ~OpenCLCtxScope()
        {
            if (!m_active) return;
            const auto queryId = m_ctx->NextQueryId(EventInfo{ m_event, EventPhase::End });

            auto item = Profiler::QueueSerial();
            MemWrite(&item->hdr.type, QueueType::GpuZoneEndSerial);
            MemWrite(&item->gpuZoneEnd.cpuTime, Profiler::GetTime());
            MemWrite(&item->gpuZoneEnd.thread, (uint32_t)m_threadID);
            MemWrite(&item->gpuZoneEnd.queryId, (uint16_t)queryId);
            MemWrite(&item->gpuZoneEnd.context, m_ctx->GetId());
            Profiler::QueueSerialFinish();
        }

        const bool m_active;
        OpenCLCtx* m_ctx;
        TTDeviceEvent m_event;
        unsigned int m_beginQueryId;
    };

    static inline OpenCLCtx* CreateCLContext()
    {
        ZoneScopedC(Color::Red);
        auto ctx = (OpenCLCtx*)tracy_malloc(sizeof(OpenCLCtx));
        new (ctx) OpenCLCtx();
        char text[40];
        int size = sprintf(text, "%d", ctx->GetId());
        TracyMessage(text, size);
        return ctx;
    }


    static inline void DestroyCLContext(OpenCLCtx* ctx)
    {
        ctx->~OpenCLCtx();
        tracy_free(ctx);
    }

}  // namespace tracy

using TracyCLCtx = tracy::OpenCLCtx*;

#define TracyCLContext() tracy::CreateCLContext();
#define TracyCLDestroy(ctx) tracy::DestroyCLContext(ctx);
#define TracyCLContextName(ctx, name, size) ctx->Name(name, size)
#if defined TRACY_HAS_CALLSTACK && defined TRACY_CALLSTACK
#  define TracyCLNamedZone(ctx, varname, name, active, threadID) static constexpr tracy::SourceLocationData TracyConcat(__tracy_gpu_source_location,TracyLine) { name, TracyFunction, TracyFile, (uint32_t)TracyLine, 0 }; tracy::OpenCLCtxScope varname(ctx, &TracyConcat(__tracy_gpu_source_location,TracyLine), TRACY_CALLSTACK, active, threadID );
#  define TracyCLNamedZoneC(ctx, varname, name, color, active, threadID) static constexpr tracy::SourceLocationData TracyConcat(__tracy_gpu_source_location,TracyLine) { name, TracyFunction, TracyFile, (uint32_t)TracyLine, color }; tracy::OpenCLCtxScope varname(ctx, &TracyConcat(__tracy_gpu_source_location,TracyLine), TRACY_CALLSTACK, active , threadID);
#  define TracyCLZone(ctx, name, threadID) TracyCLNamedZoneS(ctx, __tracy_gpu_zone, name, TRACY_CALLSTACK, true, threadID)
#  define TracyCLZoneC(ctx, name, color, threadID) TracyCLNamedZoneCS(ctx, __tracy_gpu_zone, name, color, TRACY_CALLSTACK, true, threadID)
#  define TracyCLZoneTransient( ctx, varname, name, active ) tracy::OpenCLCtxScope varname( ctx, TracyLine, TracyFile, strlen( TracyFile ), TracyFunction, strlen( TracyFunction ), name, strlen( name ), TRACY_CALLSTACK, active );
#else
#  define TracyCLNamedZone(ctx, varname, name, active, threadID) static constexpr tracy::SourceLocationData TracyConcat(__tracy_gpu_source_location,TracyLine){ name, TracyFunction, TracyFile, (uint32_t)TracyLine, 0 }; tracy::OpenCLCtxScope varname(ctx, &TracyConcat(__tracy_gpu_source_location,TracyLine), active, threadID);
#  define TracyCLNamedZoneC(ctx, varname, name, color, active, threadID) static constexpr tracy::SourceLocationData TracyConcat(__tracy_gpu_source_location,TracyLine){ name, TracyFunction, TracyFile, (uint32_t)TracyLine, color }; tracy::OpenCLCtxScope varname(ctx, &TracyConcat(__tracy_gpu_source_location,TracyLine), active, threadID);
#  define TracyCLZone(ctx, name, threadID) TracyCLNamedZone(ctx, __tracy_gpu_zone, name, true, threadID)
#  define TracyCLZoneC(ctx, name, color, threadID) TracyCLNamedZoneC(ctx, __tracy_gpu_zone, name, color, true , threadID)
#  define TracyCLZoneTransient( ctx, varname, name, active ) tracy::OpenCLCtxScope varname( ctx, TracyLine, TracyFile, strlen( TracyFile ), TracyFunction, strlen( TracyFunction ), name, strlen( name ), active );
#endif

#ifdef TRACY_HAS_CALLSTACK
#  define TracyCLNamedZoneS(ctx, varname, name, depth, active, threadID) static constexpr tracy::SourceLocationData TracyConcat(__tracy_gpu_source_location,TracyLine){ name, TracyFunction, TracyFile, (uint32_t)TracyLine, 0 }; tracy::OpenCLCtxScope varname(ctx, &TracyConcat(__tracy_gpu_source_location,TracyLine), depth, active, threadID);
#  define TracyCLNamedZoneCS(ctx, varname, name, color, depth, active, threadID) static constexpr tracy::SourceLocationData TracyConcat(__tracy_gpu_source_location,TracyLine){ name, TracyFunction, TracyFile, (uint32_t)TracyLine, color }; tracy::OpenCLCtxScope varname(ctx, &TracyConcat(__tracy_gpu_source_location,TracyLine), depth, active, threadID);
#  define TracyCLZoneS(ctx, name, depth) TracyCLNamedZoneS(ctx, __tracy_gpu_zone, name, depth, true, 0)
#  define TracyCLZoneCS(ctx, name, color, depth) TracyCLNamedZoneCS(ctx, __tracy_gpu_zone, name, color, depth, true, 0)
#  define TracyCLZoneTransientS( ctx, varname, name, depth, active ) tracy::OpenCLCtxScope varname( ctx, TracyLine, TracyFile, strlen( TracyFile ), TracyFunction, strlen( TracyFunction ), name, strlen( name ), depth, active );
#else
#  define TracyCLNamedZoneS(ctx, varname, name, depth, active) TracyCLNamedZone(ctx, varname, name, active, 0, 0)
#  define TracyCLNamedZoneCS(ctx, varname, name, color, depth, active) TracyCLNamedZoneC(ctx, varname, name, color, active, 0)
#  define TracyCLZoneS(ctx, name, depth) TracyCLZone(ctx, name)
#  define TracyCLZoneCS(ctx, name, color, depth) TracyCLZoneC(ctx, name, color)
#  define TracyCLZoneTransientS( ctx, varname, name, depth, active ) TracyCLZoneTransient( ctx, varname, name, active )
#endif

#define TracyCLNamedZoneSetEvent(varname, event) varname.SetEvent(event)
#define TracyCLZoneSetEvent(event) __tracy_gpu_zone.SetEvent(event)

#define TracyCLCollect(ctx, deviceData) ctx->Collect(deviceData)

#endif

#endif
