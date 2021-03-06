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

struct BinGz {
	u64 entry_count;
	u64 unk;
	u32 entry_id;
	// ...
};

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
}

void* (*fe_malloc)(u64, u64);
u64* (*og_load_entryid)(u64*,u32,u64*,u32,u64,u64*,u64*,u64*);
u64 (*uncompress_entryid)(u64*, u32);
void (*ktgl_io_fs_getfilepath)(char*, uint);
u32 (*original_filesize_by_idx)(BinGz*, uint);
void* (*original_bingz_get_entry_offset)(BinGz* bin, u32 index);

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

    char filename[256];
    ktgl_io_fs_getfilepath(filename, entryid);
    if(filename[0] == '\0')
        return 0;

    skyline::TcpLogger::SendRawFormat("Filename: %s\n", filename);
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

uint bingz_get_filesize_by_index(BinGz* bin, u32 index)
{
	char path[256];
    nn::util::SNPrintf(path, 256, "sdmc:/Aldebaran/forge/%d/%d", bin->entry_id, index);
    nn::fs::FileHandle file;
    Result result;
    result = nn::fs::OpenFile(&file, path, 1);
    if (result == 0) {
    	skyline::TcpLogger::SendRaw("BinGz index filesize read intercepted\n");
        s64 filesize;
        nn::fs::GetFileSize(&filesize, file);
        nn::fs::CloseFile(file);
        skyline::TcpLogger::SendRawFormat("Entry id: %d\nIndex: %d\nNew filesize: %d\n", bin->entry_id, index, filesize);
        return (u32)filesize;
    } else {
    	if(bin->entry_id == 3121)
    		skyline::TcpLogger::SendRaw("BinGz index filesize read intercepted\n");

        return original_filesize_by_idx(bin, index);
    }
}

void* bingz_get_entry_offset(BinGz* bin, u32 index)
{
	char path[256];
    nn::util::SNPrintf(path, 256, "sdmc:/Aldebaran/forge/%d/%d", bin->entry_id, index);
    nn::fs::FileHandle file;
    Result result;
    result = nn::fs::OpenFile(&file, path, 1);
    if (result == 0) {
        skyline::TcpLogger::SendRawFormat("Hijacked index found in BinGz\n", index, bin->entry_id);

        s64 filesize;
        nn::fs::GetFileSize(&filesize, file);

        void* contents;
        contents = fe_malloc(filesize, 0x10);

        nn::fs::ReadFile(file, 0, contents, filesize);
        nn::fs::CloseFile(file);

        return contents;
    } else {
        return original_bingz_get_entry_offset(bin, index);
    }
}

char* hook_get_version_string() {
    return "Aldebaran 0.1.3";
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

    A64HookFunction(
        reinterpret_cast<void*>(text + 0x4a4ca0),
        reinterpret_cast<void*>(bingz_get_filesize_by_index),
        (void**)&original_filesize_by_idx);

    A64HookFunction(
        reinterpret_cast<void*>(text + 0x4a4cc0),
        reinterpret_cast<void*>(bingz_get_entry_offset),
        (void**)&original_bingz_get_entry_offset);
    
    fe_malloc = (void* (*) (u64, u64)) text + 0x5bab80;
    ktgl_io_fs_getfilepath = (void (*)(char*, uint))text + 0x4a47d0;
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