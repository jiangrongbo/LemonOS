#include <CPU.h>
#include <Fs/TAR.h>
#include <Fs/Tmp.h>
#include <Fs/VolumeManager.h>
#include <HAL.h>
#include <Keyboard.h>
#include <Lemon.h>
#include <Liballoc.h>
#include <Logging.h>
#include <Math.h>
#include <Modules.h>
#include <Mouse.h>
#include <Net/Net.h>
#include <Objects/Service.h>
#include <PCI.h>
#include <Panic.h>
#include <Scheduler.h>
#include <SharedMemory.h>
#include <Storage/AHCI.h>
#include <Storage/ATA.h>
#include <Storage/NVMe.h>
#include <String.h>
#include <Symbols.h>
#include <Syscalls.h>
#include <Timer.h>
#include <Types.h>
#include <USB/XHCI.h>
#include <Video/Video.h>

#include <Debug.h>

uint8_t* progressBuffer = nullptr;
video_mode_t videoMode;

extern "C" void IdleProcess() {
    for (;;) {
        asm("sti");
        asm("hlt");
    }
}

void KernelProcess() {
    if (progressBuffer)
        Video::DrawBitmapImage(videoMode.width / 2 + 24 * 1, videoMode.height / 2 + 292 / 2 + 48, 24, 24,
                               progressBuffer);

    NVMe::Initialize();
    USB::XHCIController::Initialize();
    ATA::Init();
    AHCI::Init();

    if (progressBuffer)
        Video::DrawBitmapImage(videoMode.width / 2 + 24 * 2, videoMode.height / 2 + 292 / 2 + 48, 24, 24,
                               progressBuffer);

    ServiceFS::Initialize();

    Network::InitializeConnections();

    if (FsNode* node = fs::ResolvePath("/initrd/modules.cfg")) {
        char* buffer = new char[node->size + 1];

        ssize_t read = fs::Read(node, 0, node->size, buffer);
        if (read > 0) {
            buffer[read] = 0; // Null-terminate the buffer

            char* save;
            char* path = strtok_r(buffer, "\n", &save);
            while (path) {
                if (strlen(path) > 0) {
                    ModuleManager::LoadModule(path); // modules.cfg should contain a list of paths to modules
                }

                path = strtok_r(nullptr, "\n", &save);
            }
        }

        delete[] buffer;
    }

    fs::VolumeManager::MountSystemVolume();

    if (FsNode* node = fs::ResolvePath("/system/lemon")) {
        fs::VolumeManager::RegisterVolume(new fs::LinkVolume(node, "etc")); // Very hacky and cheap workaround for /etc/localtime
    }

    if (FsNode* node = fs::ResolvePath("/system/lib")) {
        fs::VolumeManager::RegisterVolume(new fs::LinkVolume(node, "lib"));
    } else {
        FsNode* initrd = fs::ResolvePath("/initrd");
        assert(initrd);

        fs::VolumeManager::RegisterVolume(new fs::LinkVolume(initrd, "lib")); // If /system/lib is not present is /initrd
    }


    if(HAL::runTests){
        ModuleManager::LoadModule("/initrd/modules/testmodule.sys");
        Log::Warning("Finished running tests. Hanging.");
        for(;;);
    }

    if (progressBuffer)
        Video::DrawBitmapImage(videoMode.width / 2 + 24 * 3, videoMode.height / 2 + 292 / 2 + 48, 24, 24,
                               progressBuffer);

    Log::Info("Loading Init Process...");
    FsNode* initFsNode = nullptr;
    char* argv[] = {"init.lef"};
    int envc = 1;
    char* envp[] = {"PATH=/initrd", nullptr};

    if (HAL::useKCon || !(initFsNode = fs::ResolvePath("/system/lemon/init.lef"))) { // Attempt to start fterm
        initFsNode = fs::ResolvePath("/initrd/fterm.lef");

        if (!initFsNode) { // Shit has really hit the fan and fterm is not on the ramdisk
            const char* panicReasons[]{"Failed to load either init task (init.lef) or fterm (fterm.lef)!"};
            KernelPanic(panicReasons, 1);
        }
    }

    Log::Write("OK");

    void* initElf = (void*)kmalloc(initFsNode->size);
    fs::Read(initFsNode, 0, initFsNode->size, (uint8_t*)initElf);

    process_t* initProc = Scheduler::CreateELFProcess(initElf, 1, argv, envc, envp);

    strcpy(initProc->workingDir, "/");
    strcpy(initProc->name, "Init");

    Scheduler::StartProcess(initProc);

    if (progressBuffer) {
        delete[] progressBuffer;
    }

    for (;;) {
        acquireLock(&Scheduler::destroyedProcessesLock);
        for (auto it = Scheduler::destroyedProcesses->begin(); it != Scheduler::destroyedProcesses->end(); it++) {
            if (!((*it)->processLock.TryAcquireWrite())) {
                Process* proc = *it;
                if(proc->addressSpace){ // Destroy the address space regardless
                    delete (proc)->addressSpace;
                    proc->addressSpace = nullptr;
                }
                if(proc->parent){
                    proc->processLock.ReleaseWrite();
                }

                delete proc;
                Scheduler::destroyedProcesses->remove(it);
            }

            if (it == Scheduler::destroyedProcesses->end()) {
                break;
            }
        }
        releaseLock(&Scheduler::destroyedProcessesLock);

        Scheduler::GetCurrentThread()->Sleep(100000);
    }
}

typedef void (*ctor_t)(void);
extern ctor_t _ctors_start[0];
extern ctor_t _ctors_end[0];
extern "C" void _init();

void InitializeConstructors() {
    unsigned ctorCount = ((uint64_t)&_ctors_end - (uint64_t)&_ctors_start) / sizeof(void*);

    for (unsigned i = 0; i < ctorCount; i++) {
        _ctors_start[i]();
    }
}

extern "C" [[noreturn]] void kmain() {
    fs::VolumeManager::Initialize();
    DeviceManager::Initialize();
    Log::LateInitialize();

    InitializeConstructors(); // Call global constructors

    DeviceManager::RegisterFSVolume();
    Log::EnableBuffer();

    videoMode = Video::GetVideoMode();

    if (debugLevelMisc >= DebugLevelVerbose) {
        Log::Info("Video Resolution: %dx%dx%d", videoMode.width, videoMode.height, videoMode.bpp);
    }

    if (videoMode.height < 600)
        Log::Warning("Small Resolution, it is recommended to use a higher resoulution if possible.");
    if (videoMode.bpp != 32)
        Log::Warning("Unsupported Colour Depth expect issues.");

    Video::DrawRect(0, 0, videoMode.width, videoMode.height, 0, 0, 0);

    Log::Info("Used RAM: %d MB", Memory::usedPhysicalBlocks * 4096 / 1024 / 1024);

    assert(fs::GetRoot());

    Log::Info("Initializing Ramdisk...");

    fs::tar::TarVolume* tar = new fs::tar::TarVolume(HAL::bootModules[0].base, HAL::bootModules[0].size, "initrd");
    fs::VolumeManager::RegisterVolume(tar);

    Log::Write("OK");

    fs::VolumeManager::RegisterVolume(new fs::Temp::TempVolume("tmp")); // Create tmpfs instance

    FsNode* initrd = fs::FindDir(fs::GetRoot(), "initrd");
    FsNode* splashFile = nullptr;
    FsNode* symbolFile = nullptr;

    if (initrd) {
        if ((splashFile = fs::FindDir(initrd, "splash.bmp"))) {
            uint32_t size = splashFile->size;
            uint8_t* buffer = new uint8_t[size];

            if (fs::Read(splashFile, 0, size, buffer) > 0)
                Video::DrawBitmapImage(videoMode.width / 2 - 620 / 2, videoMode.height / 2 - 150 / 2, 621, 150, buffer);

            delete[] buffer;
        } else
            Log::Warning("Could not load splash image");

        if ((splashFile = fs::FindDir(initrd, "pbar.bmp"))) {
            uint32_t size = splashFile->size;
            progressBuffer = new uint8_t[size];

            if (fs::Read(splashFile, 0, size, progressBuffer) > 0) {
                Video::DrawBitmapImage(videoMode.width / 2 - 24 * 4, videoMode.height / 2 + 292 / 2 + 48, 24, 24,
                                       progressBuffer);
                Video::DrawBitmapImage(videoMode.width / 2 - 24 * 3, videoMode.height / 2 + 292 / 2 + 48, 24, 24,
                                       progressBuffer);
            }
        } else
            Log::Warning("Could not load progress bar image");

        if ((symbolFile = fs::FindDir(initrd, "kernel.map"))) {
            LoadSymbolsFromFile(symbolFile);
        } else {
            KernelPanic((const char*[]){"Failed to locate kernel.map!"}, 1);
        }
    } else {
        KernelPanic((const char*[]){"initrd not mounted!"}, 1);
    }

    Video::DrawString("Copyright 2018-2021 JJ Roberts-White", 2, videoMode.height - 10, 255, 255, 255);
    Video::DrawString(Lemon::versionString, 2, videoMode.height - 20, 255, 255, 255);

    if (progressBuffer)
        Video::DrawBitmapImage(videoMode.width / 2 - 24 * 2, videoMode.height / 2 + 292 / 2 + 48, 24, 24,
                               progressBuffer);

    Log::Info("Initializing HID...");

    Mouse::Install();
    Keyboard::Install();

    Log::Info("OK");

    if (progressBuffer)
        Video::DrawBitmapImage(videoMode.width / 2 - 24 * 1, videoMode.height / 2 + 292 / 2 + 48, 24, 24,
                               progressBuffer);

    if (progressBuffer)
        Video::DrawBitmapImage(videoMode.width / 2, videoMode.height / 2 + 292 / 2 + 48, 24, 24, progressBuffer);

    Log::Info("Initializing Task Scheduler...");
    Scheduler::Initialize();
    for (;;)
        ;
}
