#include "main.hpp"

#define FunctionPointer(RETURN_TYPE, NAME, ARGS, ADDRESS) RETURN_TYPE (*NAME)ARGS = (RETURN_TYPE (*)ARGS)ADDRESS

extern "C" {
    void __custom_init(void) {  skylineMain(); }
    void __custom_fini(void) {}

    void *__dso_handle; // for linking with libc++

    // unused in the context of NSOs
    extern "C" void skylineInit(void* ctx, Handle main_thread, LoaderReturnFn saved_lr){
        *((u64*)0) = 0x69;
    }
}

nn::os::ThreadType runtimePatchThread;
char ALIGNA(0x1000) runtimePatchStack[0x7000];

// For handling exceptions
char ALIGNA(0x1000) exceptionHandlerStack[0x4000];
nn::os::UserExceptionInfo exceptionInfo;

Result (*ogRoLoadModule)(nn::ro::Module*, const void *, void *, ulong, int);

Result roLoadModuleHook(nn::ro::Module* module, const void * nro, void * buffer, ulong bufferSize, int unk) {
    Result rc = ogRoLoadModule(module, nro, buffer, bufferSize, unk);
    
    skyline::TcpLogger::LogFormat("Module \"%s\" loaded.", module->Name);

    return rc;
}

Result (*ogRoUnloadModule)(nn::ro::Module*);

Result roUnloadModuleHook(nn::ro::Module* module){

    skyline::TcpLogger::LogFormat("Module \"%s\" unloaded.", module->Name);

    return ogRoUnloadModule(module);
};

void exceptionHandler(nn::os::UserExceptionInfo* info){
    
    skyline::TcpLogger::SendRaw("Exception occured!\n");

    skyline::TcpLogger::SendRawFormat("Error description: %x\n", info->ErrorDescription);
    for(int i = 0; i < 29; i++)
        skyline::TcpLogger::SendRawFormat("X[%02i]: %" PRIx64  "\n", i, info->CpuRegisters[i].x);
    skyline::TcpLogger::SendRawFormat("FP: %" PRIx64  "\n", info->FP.x);
    skyline::TcpLogger::SendRawFormat("LR: %" PRIx64 " \n", info->LR.x);
    skyline::TcpLogger::SendRawFormat("SP: %" PRIx64   "\n", info->SP.x);
    skyline::TcpLogger::SendRawFormat("PC: %" PRIx64  "\n", info->PC.x);

    //*((u64*)0) = 0x69;
}

void* (*fe_malloc)(u64, u64);
u64* (*og_load_entryid)(u64*,u32,u64*,u32,u64,u64*,u64*,u64*);
u64 (*uncompress_entryid)(u64*, u32);

u64* load_from_forge(u64* archive_ptr, u32 entryid, u64* file_ptr, u32 seek, u64 size, u64* unk3, u64* unk4, u64* unk5) {
    char path[256];
    nn::util::SNPrintf(path, 256, "sdmc:/Aldebaran/forge/%d", entryid);
    nn::fs::FileHandle file;
    Result result;
    result = nn::fs::OpenFile(&file, path, 1);
    if (result == 0) {
        skyline::TcpLogger::SendRaw("Hijacked entry ID found\n");
        s64 filesize;
        nn::fs::GetFileSize(&filesize, file);
        u64 readSize = size == 0 ? filesize :(u64)  size;
        
        void* contents;
        if (file_ptr != 0) {
            contents = (void*) file_ptr;
        } else {
            contents = fe_malloc(readSize, 0x10);
        }

        nn::fs::ReadFile(file, (u64) seek, contents, readSize);
        nn::fs::CloseFile(file);
        
        skyline::TcpLogger::SendRawFormat("Forge ID: %d\n", entryid);
        skyline::TcpLogger::SendRawFormat("Debug: seek: %d, size: %d\n", seek, size);
        return (u64*) contents;
    } else {
        skyline::TcpLogger::SendRawFormat("Base ID: %d\n", entryid);
        
    }
    return 0;
} 

u64* forge_hook(u64* archive_ptr, u32 entryid, u64* file_ptr, u32 seek, u64 size, u64* unk3, u64* unk4, u64* unk5)
{
    u64* forgeResult = load_from_forge(archive_ptr, entryid, file_ptr, seek, size, unk3, unk4, unk5);

    if(forgeResult != 0)
        return forgeResult;

    return og_load_entryid(archive_ptr, entryid, file_ptr, seek, size, unk3, unk4, unk5);
}

u64 archive_fake_uncomp_size(u64* archive_ptr, u32 entryid)
{
    char path[256];
    nn::util::SNPrintf(path, 256, "sdmc:/Aldebaran/forge/%d", entryid);
    nn::fs::FileHandle file;
    Result result;
    result = nn::fs::OpenFile(&file, path, 1);
    if (result == 0) {
        s64 filesize;
        nn::fs::GetFileSize(&filesize, file);
        nn::fs::CloseFile(file);
        return (u64)filesize;
    } else {
        return uncompress_entryid(archive_ptr, entryid);
    }
}

char* hook_get_version_string() {
    return "Aldebaran 0.1.2";
}

void stub() {}
uint socketStub() { return 0; }

void runtimePatchMain() {  
    // init hooking setup
    A64HookInit();

    // find .text
    u64 text = memNextMapOfPerm((u64)nninitStartup, Perm_Rx); // nninitStartup can be reasonably assumed to be exported by main
    // find .rodata
    u64 rodata = memNextMapOfPerm((u64) nninitStartup, Perm_Rw);

    // override exception handler to dump info 
    nn::os::SetUserExceptionHandler(exceptionHandler, exceptionHandlerStack, sizeof(exceptionHandlerStack), &exceptionInfo);

    // nn::ro hooks
    A64HookFunction(
        reinterpret_cast<void*>(nn::ro::LoadModule),
        reinterpret_cast<void*>(roLoadModuleHook), 
        (void**) &ogRoLoadModule);
    A64HookFunction(
        reinterpret_cast<void*>(nn::ro::UnloadModule), 
        reinterpret_cast<void*>(roUnloadModuleHook), 
        (void**)&ogRoUnloadModule);

    nn::ro::Initialize();

    A64HookFunction(
        reinterpret_cast<void*>(nn::ro::Initialize), 
        reinterpret_cast<void*>(stub), 
        NULL);

    A64HookFunction(
        reinterpret_cast<void*>(text + 0x4a12b0),
        reinterpret_cast<void*>(forge_hook),
        (void**)&og_load_entryid);

    A64HookFunction(
        reinterpret_cast<void*>(text + 0x4a0b40),
        reinterpret_cast<void*>(archive_fake_uncomp_size),
        (void**)&uncompress_entryid);

    A64HookFunction(
        reinterpret_cast<void*>(text + 0x3e63e0),
        reinterpret_cast<void*>(hook_get_version_string),
        NULL);
    
    fe_malloc = (void* (*) (u64, u64)) text + 0x5bab80;
    
    // wait for nnSdk to finish booting
    nn::os::SleepThread(nn::TimeSpan::FromSeconds(1));
    //Mount SD card
    nn::fs::MountSdCardForDebug("sdmc");

    //Initialize socket for logging
    const size_t poolSize = 0x100000;
    void* socketPool = memalign(0x4000, poolSize);
    nn::socket::Initialize(socketPool, poolSize, 0x20000, 14);

    // Kill the actual function used by the game so it doesn't initialize a socket twice
    A64HookFunction(
        reinterpret_cast<void*>(text + 0xafccf0),
        reinterpret_cast<void*>(socketStub),
        NULL);


    skyline::TcpLogger::StartThread();
    skyline::Plugin::Manager::Init();
}


void skylineMain() {
    virtmemSetup();
    nn::os::CreateThread(&runtimePatchThread, runtimePatchMain, NULL, &runtimePatchStack, sizeof(runtimePatchStack), 20, 3);
    nn::os::StartThread(&runtimePatchThread);
}