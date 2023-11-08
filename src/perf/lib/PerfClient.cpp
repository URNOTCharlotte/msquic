/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    QUIC Perf Client Implementation

--*/

#include "PerfClient.h"

#ifdef QUIC_CLOG
#include "PerfClient.cpp.clog.h"
#endif

const char PERF_CLIENT_OPTIONS_TEXT[] =
"\n"
"Usage (client): secnetperf -target:<hostname/ip> [client options]\n"
"\n"
"Client Options:\n"
"\n"
"  Remote options:\n"
"  -ip:<0/4/6>              A hint for the resolving the hostname to an IP address. (def:0)\n"
"  -port:<####>             The UDP port of the server. (def:%u)\n"
"  -cibir:<hex_bytes>       A CIBIR well-known idenfitier.\n"
"  -incrementtarget:<0/1>   Append unique ID to target hostname for each worker (def:0).\n"
"\n"
"  Local options:\n"
"  -threads:<####>          The max number of worker threads to use.\n"
"  -affinitize:<0/1>        Affinitizes worker threads to a core. (def:0)\n"
#ifdef QUIC_COMPARTMENT_ID
"  -comp:<####>             The network compartment ID to run in.\n"
#endif
"  -bind:<addr>             The local IP address(es)/port(s) to bind to.\n"
"  -share:<0/1>             Shares the same local bindings. (def:0)\n"
"\n"
"  Config options:\n"
"  -tcp:<0/1>               Disables/enables TCP usage (instead of QUIC). (def:0)\n"
"  -encrypt:<0/1>           Disables/enables encryption. (def:1)\n"
"  -pacing:<0/1>            Disables/enables send pacing. (def:1)\n"
"  -sendbuf:<0/1>           Disables/enables send buffering. (def:0)\n"
"  -ptput:<0/1>             Print throughput information. (def:0)\n"
"  -pconn:<0/1>             Print connection statistics. (def:0)\n"
"  -pstream:<0/1>           Print stream statistics. (def:0)\n"
"  -platency<0/1>           Print latency statistics. (def:0)\n"
"\n"
"  Scenario options:\n"
"  -conns:<####>            The number of connections to use. (def:1)\n"
"  -streams:<####>          The number of streams to send on at a time. (def:0)\n"
"  -upload:<####>           The length of bytes to send on each stream. (def:0)\n"
"  -download:<####>         The length of bytes to receive on each stream. (def:0)\n"
"  -timed:<0/1>             Indicates the upload/download args are times (in ms). (def:0)\n"
//"  -inline:<0/1>            Create new streams on callbacks. (def:0)\n"
"  -rconn:<0/1>             Repeat the scenario at the connection level. (def:0)\n"
"  -rstream:<0/1>           Repeat the scenario at the stream level. (def:0)\n"
"  -runtime:<####>          The total runtime (in ms). Only relevant for repeat scenarios. (def:0)\n"
"\n";

static void PrintHelp() {
    WriteOutput(
        PERF_CLIENT_OPTIONS_TEXT,
        PERF_DEFAULT_PORT
        );
}

QUIC_STATUS
PerfClient::Init(
    _In_ int argc,
    _In_reads_(argc) _Null_terminated_ char* argv[]
    ) {
    if (argc > 0 && (IsArg(argv[0], "?") || IsArg(argv[0], "help"))) {
        PrintHelp();
        return QUIC_STATUS_INVALID_PARAMETER;
    }

    if (!Configuration.IsValid()) {
        return Configuration.GetInitStatus();
    }

    //
    // Remote target/server options
    //

    const char* target;
    if (!TryGetValue(argc, argv, "target", &target) &&
        !TryGetValue(argc, argv, "server", &target)) {
        WriteOutput("Must specify '-target' argument!\n");
        PrintHelp();
        return QUIC_STATUS_INVALID_PARAMETER;
    }

    size_t Len = strlen(target);
    Target.reset(new(std::nothrow) char[Len + 1]);
    if (!Target.get()) {
        return QUIC_STATUS_OUT_OF_MEMORY;
    }
    CxPlatCopyMemory(Target.get(), target, Len);
    Target[Len] = '\0';

    uint16_t Ip;
    if (TryGetValue(argc, argv, "ip", &Ip)) {
        switch (Ip) {
        case 4: TargetFamily = QUIC_ADDRESS_FAMILY_INET; break;
        case 6: TargetFamily = QUIC_ADDRESS_FAMILY_INET6; break;
        }
    }

    TryGetValue(argc, argv, "port", &TargetPort);
    TryGetValue(argc, argv, "incrementtarget", &IncrementTarget);

    const char* CibirBytes = nullptr;
    if (TryGetValue(argc, argv, "cibir", &CibirBytes)) {
        CibirId[0] = 0; // offset
        if ((CibirIdLength = DecodeHexBuffer(CibirBytes, 6, CibirId+1)) == 0) {
            WriteOutput("Cibir ID must be a hex string <= 6 bytes.\n");
            return QUIC_STATUS_INVALID_PARAMETER;
        }
    }

    //
    // Local address and execution configuration options
    //

    WorkerCount = CxPlatProcActiveCount();
    TryGetValue(argc, argv, "threads", &WorkerCount);
    TryGetValue(argc, argv, "workers", &WorkerCount);
    TryGetValue(argc, argv, "affinitize", &AffinitizeWorkers);

#ifdef QUIC_COMPARTMENT_ID
    TryGetValue(argc, argv, "comp", &CompartmentId);
#endif

    TryGetValue(argc, argv, "share", &SpecificLocalAddresses);

    char* LocalAddress = (char*)GetValue(argc, argv, "bind");
    if (LocalAddress != nullptr) {
        SpecificLocalAddresses = true;
        uint32_t Index = 0;
        while (LocalAddress && Index < WorkerCount) {
            char* AddrEnd = strchr(LocalAddress, ',');
            if (AddrEnd) {
                *AddrEnd = '\0';
                AddrEnd++;
            }
            if (!ConvertArgToAddress(LocalAddress, 0, &Workers[Index++].LocalAddr.SockAddr)) {
                WriteOutput("Failed to decode bind IP address: '%s'!\nMust be *, a IPv4 or a IPv6 address.\n", LocalAddress);
                PrintHelp();
                return QUIC_STATUS_INVALID_PARAMETER;
            }
            LocalAddress = AddrEnd;
        }

        for (uint32_t i = Index; i < WorkerCount; ++i) {
            Workers[i].LocalAddr.SockAddr = Workers[(i-Index)%Index].LocalAddr.SockAddr;
        }
    }

    //
    // General configuration options
    //

    TryGetValue(argc, argv, "tcp", &UseTCP);
    TryGetValue(argc, argv, "encrypt", &UseEncryption);
    TryGetValue(argc, argv, "pacing", &UsePacing);
    TryGetValue(argc, argv, "sendbuf", &UseSendBuffering);
    TryGetValue(argc, argv, "ptput", &PrintThroughput);
    TryGetValue(argc, argv, "pconnection", &PrintConnections);
    TryGetValue(argc, argv, "pconn", &PrintConnections);
    TryGetValue(argc, argv, "pstream", &PrintStreams);
    TryGetValue(argc, argv, "platency", &PrintLatency);
    TryGetValue(argc, argv, "plat", &PrintLatency);

    if (UseSendBuffering || !UsePacing) { // Update settings if non-default
        MsQuicSettings Settings;
        Configuration.GetSettings(Settings);
        if (!UseSendBuffering) {
            Settings.SetSendBufferingEnabled(UseSendBuffering != 0);
        }
        if (!UsePacing) {
            Settings.SetPacingEnabled(UsePacing != 0);
        }
        Configuration.SetSettings(Settings);
    }

    //
    // Scenario options
    //

    TryGetValue(argc, argv, "conns", &ConnectionCount);
    TryGetValue(argc, argv, "requests", &StreamCount);
    TryGetValue(argc, argv, "streams", &StreamCount);
    TryGetValue(argc, argv, "iosize", &IoSize);
    if (IoSize < 256) {
        WriteOutput("'iosize' too small'!\n");
        return QUIC_STATUS_INVALID_PARAMETER;
    }
    TryGetValue(argc, argv, "request", &Upload);
    TryGetValue(argc, argv, "upload", &Upload);
    TryGetValue(argc, argv, "up", &Upload);
    TryGetValue(argc, argv, "response", &Download);
    TryGetValue(argc, argv, "download", &Download);
    TryGetValue(argc, argv, "down", &Download);
    TryGetValue(argc, argv, "timed", &Timed);
    //TryGetValue(argc, argv, "inline", &SendInline);
    TryGetValue(argc, argv, "rconn", &RepeatConnections);
    TryGetValue(argc, argv, "rstream", &RepeatStreams);
    TryGetValue(argc, argv, "runtime", &RunTime);
    TryGetValue(argc, argv, "time", &RunTime);
    TryGetValue(argc, argv, "run", &RunTime);

    if ((RepeatConnections || RepeatStreams) && !RunTime) {
        WriteOutput("Must specify a 'runtime' if using a repeat parameter!\n");
        return QUIC_STATUS_INVALID_PARAMETER;
    }

    if (UseTCP) {
        if (!UseEncryption) {
            WriteOutput("TCP mode doesn't support disabling encryption!\n");
            return QUIC_STATUS_INVALID_PARAMETER;
        }
    }

    if ((Upload || Download) && !StreamCount) {
        StreamCount = 1; // Just up/down args imply they want a stream
    }

    //
    // Other state initialization
    //

    if (UseTCP) {
        Engine =
            new(std::nothrow) TcpEngine(
                nullptr,
                PerfClientConnection::TcpConnectCallback,
                PerfClientConnection::TcpReceiveCallback,
                PerfClientConnection::TcpSendCompleteCallback);
    }

    RequestBuffer.Init(IoSize, Timed ? UINT64_MAX : Download);
    if (PrintLatency) {
        if (RunTime) {
            MaxLatencyIndex = ((uint64_t)RunTime / 1000) * PERF_MAX_REQUESTS_PER_SECOND;
            if (MaxLatencyIndex > (UINT32_MAX / sizeof(uint32_t))) {
                MaxLatencyIndex = UINT32_MAX / sizeof(uint32_t);
                WriteOutput("Warning! Limiting request latency tracking to %llu requests\n",
                    (unsigned long long)MaxLatencyIndex);
            }
        } else {
            MaxLatencyIndex = ConnectionCount * StreamCount;
        }

        LatencyValues = UniquePtr<uint32_t[]>(new(std::nothrow) uint32_t[(size_t)MaxLatencyIndex]);
        if (LatencyValues == nullptr) {
            return QUIC_STATUS_OUT_OF_MEMORY;
        }
        CxPlatZeroMemory(LatencyValues.get(), (size_t)(sizeof(uint32_t) * MaxLatencyIndex));
    }

    return QUIC_STATUS_SUCCESS;
}

static void AppendIntToString(char* String, uint8_t Value) {
    const char* Hex = "0123456789ABCDEF";
    String[0] = Hex[(Value >> 4) & 0xF];
    String[1] = Hex[Value & 0xF];
    String[2] = '\0';
}

QUIC_STATUS
PerfClient::Start(
    _In_ CXPLAT_EVENT* StopEvent
    ) {
    CompletionEvent = StopEvent;

    //
    // Resolve the remote address to connect to (to optimize the HPS metric).
    //
    QUIC_STATUS Status;
    CXPLAT_DATAPATH* Datapath = nullptr;
    if (QUIC_FAILED(Status = CxPlatDataPathInitialize(0, nullptr, nullptr, nullptr, &Datapath))) {
        WriteOutput("Failed to initialize datapath for resolution!\n");
        return Status;
    }
    QUIC_ADDR RemoteAddr;
    Status = CxPlatDataPathResolveAddress(Datapath, Target.get(), &RemoteAddr);
    CxPlatDataPathUninitialize(Datapath);
    if (QUIC_FAILED(Status)) {
        WriteOutput("Failed to resolve remote address!\n");
        return Status;
    }

    //
    // Configure and start all the workers.
    //
    CXPLAT_THREAD_CONFIG ThreadConfig = {
        (uint16_t)(AffinitizeWorkers ? CXPLAT_THREAD_FLAG_SET_AFFINITIZE : CXPLAT_THREAD_FLAG_NONE),
        0,
        "Perf Worker",
        PerfClientWorker::s_WorkerThread,
        nullptr
    };
    const size_t TargetLen = strlen(Target.get());
    for (uint32_t i = 0; i < WorkerCount; ++i) {
        while (!CxPlatProcIsActive(ThreadConfig.IdealProcessor)) {
            ++ThreadConfig.IdealProcessor;
        }

        auto Worker = &Workers[i];
        Worker->Processor = ThreadConfig.IdealProcessor++;
        ThreadConfig.Context = Worker;
        Worker->RemoteAddr.SockAddr = RemoteAddr;
        Worker->RemoteAddr.SetPort(TargetPort);

        // Build up target hostname.
        Worker->Target.reset(new(std::nothrow) char[TargetLen + 10]);
        CxPlatCopyMemory(Worker->Target.get(), Target.get(), TargetLen);
        if (IncrementTarget) {
            AppendIntToString(Worker->Target.get() + TargetLen, (uint8_t)Worker->Processor);
        } else {
            Worker->Target.get()[TargetLen] = '\0';
        }
        Worker->Target.get()[TargetLen] = '\0';

        Status = CxPlatThreadCreate(&ThreadConfig, &Workers[i].Thread);
        if (QUIC_FAILED(Status)) {
            WriteOutput("Failed to start worker thread on processor %hu!\n", Worker->Processor);
            return Status;
        }
        Workers[i].ThreadStarted = true;
    }

    //
    // Queue the connections on the workers.
    //
    for (uint32_t i = 0; i < ConnectionCount; ++i) {
        Workers[i % WorkerCount].QueueNewConnection();
    }

    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS
PerfClient::Wait(
    _In_ int Timeout
    ) {
    if (Timeout == 0) {
        Timeout = RunTime;
    }

    if (Timeout) {
        CxPlatEventWaitWithTimeout(*CompletionEvent, Timeout);
    } else {
        CxPlatEventWaitForever(*CompletionEvent);
    }

    Running = false;
    for (uint32_t i = 0; i < WorkerCount; ++i) {
        Workers[i].Uninitialize();
    }

    WriteOutput(
        "Completed %llu connections and %llu streams!\n",
        (unsigned long long)GetConnectionsCompleted(),
        (unsigned long long)GetStreamsCompleted());

    return QUIC_STATUS_SUCCESS;
}

void
PerfClient::GetExtraDataMetadata(
    _Out_ PerfExtraDataMetadata* Result
    )
{
    Result->TestType = PerfTestType::Client;
    if (!MaxLatencyIndex) {
        Result->ExtraDataLength = 0; // Not capturing this extra data
    } else {
        const auto DataLength =
            sizeof(RunTime) +
            sizeof(CurLatencyIndex) +
            (LatencyCount * sizeof(uint32_t));
        CXPLAT_FRE_ASSERT(DataLength <= UINT32_MAX); // TODO Limit values properly
        Result->ExtraDataLength = (uint32_t)DataLength;
    }
}

QUIC_STATUS
PerfClient::GetExtraData(
    _Out_writes_bytes_(*Length) uint8_t* Data,
    _Inout_ uint32_t* Length
    )
{
    CXPLAT_FRE_ASSERT(MaxLatencyIndex); // Shouldn't be called if we're not tracking latency
    CXPLAT_FRE_ASSERT(*Length >= sizeof(RunTime) + sizeof(CurLatencyIndex));
    CxPlatCopyMemory(Data, &RunTime, sizeof(RunTime));
    Data += sizeof(RunTime);
    uint64_t LatencyCount = (*Length - sizeof(RunTime) - sizeof(LatencyCount)) / sizeof(uint32_t);
    CxPlatCopyMemory(Data, &LatencyCount, sizeof(LatencyCount));
    Data += sizeof(CurLatencyIndex);
    CxPlatCopyMemory(Data, LatencyValues.get(), LatencyCount * sizeof(uint32_t));
    return QUIC_STATUS_SUCCESS;
}

void
PerfClientWorker::WorkerThread() {
#ifdef QUIC_COMPARTMENT_ID
    if (Client->CompartmentId != UINT16_MAX) {
        NETIO_STATUS status;
        if (!NETIO_SUCCESS(status = QuicCompartmentIdSetCurrent(Client->CompartmentId))) {
            WriteOutput("Failed to set compartment ID = %d: 0x%x\n", Client->CompartmentId, status);
            return;
        }
    }
#endif

    while (Client->Running) {
        while (ConnectionsCreated < ConnectionsQueued) {
            StartNewConnection();
        }
        WakeEvent.WaitForever();
    }
}

void
PerfClientWorker::OnConnectionComplete() {
    InterlockedIncrement64((int64_t*)&ConnectionsCompleted);
    InterlockedDecrement64((int64_t*)&ConnectionsActive);
    if (Client->RepeatConnections) {
        QueueNewConnection();
    } else {
        if (!ConnectionsActive && ConnectionsCreated == ConnectionsQueued) {
            Client->OnConnectionsComplete();
        }
    }
}

void
PerfClientWorker::StartNewConnection() {
    InterlockedIncrement64((int64_t*)&ConnectionsCreated);
    InterlockedIncrement64((int64_t*)&ConnectionsActive);
    ConnectionAllocator.Alloc(*Client, *this)->Initialize();
}

PerfClientConnection::~PerfClientConnection() {
    if (Client.UseTCP) {
        if (TcpConn) { TcpConn->Close(); } // TODO - Free to pool instead
    } else {
        if (Handle) { MsQuic->ConnectionClose(Handle); }
    }
}

void
PerfClientConnection::Initialize() {
    if (Client.UseTCP) {
        auto CredConfig = MsQuicCredentialConfig(QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION);
        TcpConn =
            Worker.TcpConnectionAllocator.Alloc(
                Client.Engine,
                &CredConfig,
                Client.TargetFamily,
                Worker.Target.get(),
                Worker.RemoteAddr.GetPort(),
                Worker.LocalAddr.GetFamily() != QUIC_ADDRESS_FAMILY_UNSPEC ? &Worker.LocalAddr.SockAddr : nullptr,
                this);
        if (!TcpConn->IsInitialized()) {
            Worker.ConnectionAllocator.Free(this);
            return;
        }

    } else {
        BOOLEAN Value;
        if (QUIC_FAILED(
            MsQuic->ConnectionOpen(
                Client.Registration,
                PerfClientConnection::s_ConnectionCallback,
                this,
                &Handle))) {
            Worker.ConnectionAllocator.Free(this);
            return;
        }

        QUIC_STATUS Status;
        if (!Client.UseEncryption) {
            Value = TRUE;
            Status =
                MsQuic->SetParam(
                    Handle,
                    QUIC_PARAM_CONN_DISABLE_1RTT_ENCRYPTION,
                    sizeof(Value),
                    &Value);
            if (QUIC_FAILED(Status)) {
                WriteOutput("SetDisable1RttEncryption failed, 0x%x\n", Status);
                Worker.ConnectionAllocator.Free(this);
                return;
            }
        }

        if (Client.CibirIdLength) {
            Status =
                MsQuic->SetParam(
                    Handle,
                    QUIC_PARAM_CONN_CIBIR_ID,
                    Client.CibirIdLength+1,
                    Client.CibirId);
            if (QUIC_FAILED(Status)) {
                WriteOutput("SetCibirId failed, 0x%x\n", Status);
                Worker.ConnectionAllocator.Free(this);
                return;
            }
        }

        if (Client.SpecificLocalAddresses) {
            Value = TRUE;
            Status =
                MsQuic->SetParam(
                    Handle,
                    QUIC_PARAM_CONN_SHARE_UDP_BINDING,
                    sizeof(Value),
                    &Value);
            if (QUIC_FAILED(Status)) {
                WriteOutput("SetShareUdpBinding failed, 0x%x\n", Status);
                Worker.ConnectionAllocator.Free(this);
                return;
            }

            if (Worker.LocalAddr.GetFamily() != QUIC_ADDRESS_FAMILY_UNSPEC) {
                Status =
                    MsQuic->SetParam(
                        Handle,
                        QUIC_PARAM_CONN_LOCAL_ADDRESS,
                        sizeof(QUIC_ADDR),
                        &Worker.LocalAddr);
                if (QUIC_FAILED(Status)) {
                    WriteOutput("SetLocalAddr failed!\n");
                    Worker.ConnectionAllocator.Free(this);
                    return;
                }
            }
        }

        Status =
            MsQuic->ConnectionStart(
                Handle,
                Client.Configuration,
                Client.TargetFamily,
                Worker.Target.get(),
                Worker.RemoteAddr.GetPort());
        if (QUIC_FAILED(Status)) {
            WriteOutput("Start failed, 0x%x\n", Status);
            Worker.ConnectionAllocator.Free(this);
            return;
        }

        if (Client.SpecificLocalAddresses && Worker.LocalAddr.GetFamily() == QUIC_ADDRESS_FAMILY_UNSPEC) {
            uint32_t Size = sizeof(QUIC_ADDR);
            Status = // FYI, this can race with ConnectionStart failing
                MsQuic->GetParam(
                    Handle,
                    QUIC_PARAM_CONN_LOCAL_ADDRESS,
                    &Size,
                    &Worker.LocalAddr);
            if (QUIC_FAILED(Status)) {
                WriteOutput("GetLocalAddr failed!\n");
                return;
            }
        }
    }
}

void
PerfClientConnection::OnConnectionComplete() {
    InterlockedIncrement64((int64_t*)&Worker.ConnectionsConnected);
    if (!Client.StreamCount) {
        MsQuic->ConnectionShutdown(Handle, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
    } else {
        for (uint32_t i = 0; i < Client.StreamCount; ++i) {
            StartNewStream();
        }
    }
}

void
PerfClientConnection::OnShutdownComplete() {
    if (Client.PrintConnections) {
        QuicPrintConnectionStatistics(MsQuic, Handle);
    }
    Worker.OnConnectionComplete();
    Worker.ConnectionAllocator.Free(this);
}

void
PerfClientConnection::StartNewStream() {
    StreamsCreated++;
    StreamsActive++;
    auto Stream = Worker.StreamAllocator.Alloc(*this);
    if (Client.UseTCP) {
        Stream->Entry.Signature = (uint32_t)Worker.StreamsStarted;
        StreamTable.Insert(&Stream->Entry);
    } else {
        if (QUIC_FAILED(
            MsQuic->StreamOpen(
                Handle,
                QUIC_STREAM_OPEN_FLAG_NONE,
                PerfClientStream::s_StreamCallback,
                Stream,
                &Stream->Handle))) {
            Worker.StreamAllocator.Free(Stream);
            return;
        }
    }

    InterlockedIncrement64((int64_t*)&Worker.StreamsStarted);
    Stream->Send();
}

PerfClientStream*
PerfClientConnection::GetTcpStream(uint32_t ID) {
    return (PerfClientStream*)StreamTable.Lookup(ID);
}

void
PerfClientConnection::OnStreamShutdownComplete() {
    StreamsActive--;
    if (Client.RepeatStreams) {
        while (StreamsActive < Client.StreamCount) {
            StartNewStream();
        }
    } else {
        if (!StreamsActive && StreamsCreated == Client.StreamCount) {
            MsQuic->ConnectionShutdown(Handle, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
        }
    }
}

QUIC_STATUS
PerfClientConnection::ConnectionCallback(
    _Inout_ QUIC_CONNECTION_EVENT* Event
    ) {
    switch (Event->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
        OnConnectionComplete();
        break;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        OnShutdownComplete();
        break;
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

void
PerfClientConnection::TcpConnectCallback(
    _In_ TcpConnection* Connection,
    bool IsConnected
    ) {
    auto This = (PerfClientConnection*)Connection->Context;
    if (!IsConnected) {
        This->OnConnectionComplete();
    } else {
        // TODO - Iterate through streams
        This->OnShutdownComplete();
    }
}
void
PerfClientConnection::TcpSendCompleteCallback(
    _In_ TcpConnection* Connection,
    _In_ TcpSendData* SendDataChain
    ) {
    auto This = (PerfClientConnection*)Connection->Context;
    while (SendDataChain) {
        auto Data = SendDataChain;
        SendDataChain = Data->Next;

        auto Stream = This->GetTcpStream(SendDataChain->StreamId);
        if (Stream) {
            Stream->OnSendComplete(Data->Length, FALSE);
            if ((Data->Fin || Data->Abort) && !Stream->SendEndTime) {
                Stream->SendEndTime = CxPlatTimeUs64();
                if (Stream->RecvEndTime) {
                    Stream->OnStreamShutdownComplete();
                }
            }
        }
        This->Worker.TcpSendDataAllocator.Free(Data);
    }
}

void
PerfClientConnection::TcpReceiveCallback(
    _In_ TcpConnection* Connection,
    uint32_t StreamID,
    bool Open,
    bool Fin,
    bool Abort,
    uint32_t Length,
    _In_ uint8_t* Buffer
    ) {
    UNREFERENCED_PARAMETER(Open);
    UNREFERENCED_PARAMETER(Buffer);
    auto This = (PerfClientConnection*)Connection->Context;
    auto Stream = This->GetTcpStream(StreamID);
    if (Stream) {
        Stream->OnReceive(Length, Fin);
        if (Abort) {
            if (!Stream->RecvEndTime) { Stream->RecvEndTime = CxPlatTimeUs64(); }
            if (Stream->SendEndTime) {
                Stream->OnStreamShutdownComplete();
            }
        }
    }
}

QUIC_STATUS
PerfClientStream::StreamCallback(
    _Inout_ QUIC_STREAM_EVENT* Event
    ) {
    switch (Event->Type) {
    case QUIC_STREAM_EVENT_RECEIVE:
        OnReceive(Event->RECEIVE.TotalBufferLength, Event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN);
        break;
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
        OnSendComplete(((QUIC_BUFFER*)Event->SEND_COMPLETE.ClientContext)->Length, Event->SEND_COMPLETE.Canceled);
        break;
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
        if (!RecvEndTime) { RecvEndTime = CxPlatTimeUs64(); }
        MsQuic->StreamShutdown(Handle, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        break;
    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
        if (!SendEndTime) { SendEndTime = CxPlatTimeUs64(); }
        MsQuic->StreamShutdown(Handle, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND, 0);
        SendComplete = true;
        break;
    case QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE:
        SendEndTime = CxPlatTimeUs64();
        if (Connection.Client.PrintStreams) {
            QuicPrintStreamStatistics(MsQuic, Handle);
        }
        break;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        OnStreamShutdownComplete();
        break;
    case QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE:
        if (Connection.Client.Upload && !Connection.Client.UseSendBuffering &&
            IdealSendBuffer != Event->IDEAL_SEND_BUFFER_SIZE.ByteCount) {
            IdealSendBuffer = Event->IDEAL_SEND_BUFFER_SIZE.ByteCount;
            Send();
        }
    default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
}

void
PerfClientStream::Send() {
    auto& Client = Connection.Client;
    while (!SendComplete && BytesOutstanding < IdealSendBuffer) {

        const uint64_t BytesLeftToSend =
            Client.Timed ?
                UINT64_MAX : // Timed sends forever
                (Client.Upload ? (Client.Upload - BytesSent) : sizeof(uint64_t));
        uint32_t DataLength = Client.IoSize;
        QUIC_BUFFER* Buffer = Client.RequestBuffer;
        QUIC_SEND_FLAGS Flags = QUIC_SEND_FLAG_START;

        if ((uint64_t)DataLength >= BytesLeftToSend) {
            DataLength = (uint32_t)BytesLeftToSend;
            LastBuffer.Buffer = Buffer->Buffer;
            LastBuffer.Length = DataLength;
            Buffer = &LastBuffer;
            Flags |= QUIC_SEND_FLAG_FIN;
            SendComplete = true;

        } else if (Client.Timed &&
                   CxPlatTimeDiff64(StartTime, CxPlatTimeUs64()) >= MS_TO_US(Client.Upload)) {
            Flags |= QUIC_SEND_FLAG_FIN;
            SendComplete = true;
        }

        BytesSent += DataLength;
        BytesOutstanding += DataLength;

        if (Client.UseTCP) {
            auto SendData = Connection.Worker.TcpSendDataAllocator.Alloc();
            SendData->StreamId = (uint32_t)Entry.Signature;
            SendData->Open = BytesSent == DataLength ? TRUE : FALSE;
            SendData->Buffer = Buffer->Buffer;
            SendData->Length = DataLength;
            SendData->Fin = Flags & QUIC_SEND_FLAG_FIN;
            Connection.TcpConn->Send(SendData);
        } else {
            MsQuic->StreamSend(Handle, Buffer, 1, Flags, Buffer);
        }
    }
}

void
PerfClientStream::OnSendComplete(
    _In_ uint32_t Length,
    _In_ bool Canceled
    ) {
    BytesOutstanding -= Length;
    if (!Canceled) {
        BytesAcked += Length;
        Send();
    }
}

void
PerfClientStream::OnReceive(
    _In_ uint64_t Length,
    _In_ bool Finished
    ) {
    BytesReceived += Length;

    uint64_t Now = 0;
    if (!RecvStartTime) {
        Now = CxPlatTimeUs64();
        RecvStartTime = Now;
    }

    if (Finished) {
        if (Now == 0) Now = CxPlatTimeUs64();
        RecvEndTime = Now;
    } if (Connection.Client.Timed) {
        if (Now == 0) Now = CxPlatTimeUs64();
        if (CxPlatTimeDiff64(RecvStartTime, Now) >= MS_TO_US(Connection.Client.Download)) {
            RecvEndTime = Now;
            if (Connection.Client.UseTCP) {
                auto SendData = Connection.Worker.TcpSendDataAllocator.Alloc();
                SendData->StreamId = (uint32_t)Entry.Signature;
                SendData->Abort = true;
                Connection.TcpConn->Send(SendData);
                if (SendEndTime) {
                    OnStreamShutdownComplete();
                }
            } else {
                MsQuic->StreamShutdown(Handle, QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE, 0);
            }
        }
    }
}

void
PerfClientStream::OnStreamShutdownComplete() {
    auto& Client = Connection.Client;
    auto SendSuccess = SendEndTime != 0;
    if (Client.Upload) {
        const auto TotalBytes = BytesAcked;
        if (TotalBytes < sizeof(uint64_t) || (!Client.Timed && TotalBytes < Client.Upload)) {
            SendSuccess = false;
        }

        if (Client.PrintThroughput && SendSuccess) {
            const auto ElapsedMicroseconds = SendEndTime - StartTime;
            const auto Rate = (uint32_t)((TotalBytes * 1000 * 1000 * 8) / (1000 * ElapsedMicroseconds));
            WriteOutput(
                "  Upload: %llu bytes @ %u kbps (%u.%03u ms).\n",
                (unsigned long long)TotalBytes,
                Rate,
                (uint32_t)(ElapsedMicroseconds / 1000),
                (uint32_t)(ElapsedMicroseconds % 1000));
        }
    }

    auto RecvSuccess = RecvStartTime !=0 && RecvEndTime != 0;
    if (Client.Download) {
        const auto TotalBytes = BytesReceived;
        if (TotalBytes == 0 || (!Client.Timed && TotalBytes < Client.Download)) {
            RecvSuccess = false;
        }

        if (Client.PrintThroughput && RecvSuccess) {
            const auto ElapsedMicroseconds = RecvEndTime - RecvStartTime;
            const auto Rate = (uint32_t)((TotalBytes * 1000 * 1000 * 8) / (1000 * ElapsedMicroseconds));
            WriteOutput(
                "Download: %llu bytes @ %u kbps (%u.%03u ms).\n",
                (unsigned long long)TotalBytes,
                Rate,
                (uint32_t)(ElapsedMicroseconds / 1000),
                (uint32_t)(ElapsedMicroseconds % 1000));
        }
    }

    if (SendSuccess && RecvSuccess) {
        const auto Index = (uint64_t)InterlockedIncrement64((int64_t*)&Connection.Client.CurLatencyIndex) - 1;
        if (Index < Client.MaxLatencyIndex) {
            const auto Latency = CxPlatTimeDiff64(StartTime, RecvEndTime);
            Client.LatencyValues[(size_t)Index] = Latency > UINT32_MAX ? UINT32_MAX : (uint32_t)Latency;
            InterlockedIncrement64((int64_t*)&Connection.Client.LatencyCount);
        }
        InterlockedIncrement64((int64_t*)&Connection.Worker.StreamsCompleted);
    }

    auto& Conn = Connection;
    Connection.Worker.StreamAllocator.Free(this);
    Conn.OnStreamShutdownComplete();
}
