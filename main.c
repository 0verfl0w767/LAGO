#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define BUFFER_SIZE (512 * 1024 * 1024)  // 512MB
#define PROGRESS_BAR_WIDTH 50

typedef struct {
    int index;
    char path[64];
    char model[128];
    LONGLONG size;
} DiskInfo;

typedef struct {
    BYTE* buffer;
    DWORD bytes;
    volatile BOOL ready;
    volatile BOOL done;
    HANDLE hDisk;
    HANDLE hImage;
    LONGLONG totalSize;
    volatile LONGLONG totalWritten;
} PIPELINE;

void draw_progress(double progress, double mbPerSec, double secLeft) {
    int filled = (int)(progress * PROGRESS_BAR_WIDTH);
    printf("\r[");
    for (int i = 0; i < PROGRESS_BAR_WIDTH; i++)
        printf(i < filled ? "#" : " ");
    printf("] %.1f%% %.2f MB/s ETA %.1fs", progress * 100, mbPerSec, secLeft);
    fflush(stdout);
}

LONGLONG get_disk_size(const char* path) {
    HANDLE hDisk = CreateFileA(path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hDisk == INVALID_HANDLE_VALUE) return -1;

    GET_LENGTH_INFORMATION info;
    DWORD bytesReturned;
    BOOL ok = DeviceIoControl(hDisk, IOCTL_DISK_GET_LENGTH_INFO,
        NULL, 0, &info, sizeof(info), &bytesReturned, NULL);
    CloseHandle(hDisk);
    return ok ? info.Length.QuadPart : -1;
}

int enum_disks(DiskInfo* list, int maxCount) {
    int count = 0;
    char path[64];
    for (int i = 0; i < 32 && count < maxCount; i++) {
        snprintf(path, sizeof(path), "\\\\.\\PhysicalDrive%d", i);
        HANDLE hDisk = CreateFileA(path, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (hDisk == INVALID_HANDLE_VALUE)
            continue;

        STORAGE_PROPERTY_QUERY query = { StorageDeviceProperty, PropertyStandardQuery };
        BYTE buffer[1024];
        DWORD bytesReturned;

        if (DeviceIoControl(hDisk, IOCTL_STORAGE_QUERY_PROPERTY,
            &query, sizeof(query),
            buffer, sizeof(buffer),
            &bytesReturned, NULL)) {
            STORAGE_DEVICE_DESCRIPTOR* devDesc = (STORAGE_DEVICE_DESCRIPTOR*)buffer;
            char* vendor = devDesc->VendorIdOffset ? (char*)buffer + devDesc->VendorIdOffset : "";
            char* product = devDesc->ProductIdOffset ? (char*)buffer + devDesc->ProductIdOffset : "";

            snprintf(list[count].path, sizeof(list[count].path), "%s", path);
            snprintf(list[count].model, sizeof(list[count].model), "%s %s", vendor, product);
            list[count].size = get_disk_size(path);
            list[count].index = i;
            count++;
        }
        CloseHandle(hDisk);
    }
    return count;
}

void print_disks(DiskInfo* list, int count) {
    printf("\n연결된 디스크 목록:\n");
    printf("---------------------------------------------\n");
    for (int i = 0; i < count; i++) {
        double gb = list[i].size / (1024.0 * 1024 * 1024);
        printf("[%d] %-25s  %.2f GB  (%s)\n", i + 1,
            list[i].path, gb, list[i].model);
    }
    printf("---------------------------------------------\n");
}

DWORD WINAPI ReaderThread(LPVOID param) {
    PIPELINE* pipe = (PIPELINE*)param;
    LONGLONG offset = 0;
    DWORD bytesRead;

    while (offset < pipe->totalSize) {
        while (pipe->ready) Sleep(1);

        if (!ReadFile(pipe->hDisk, pipe->buffer, BUFFER_SIZE, &bytesRead, NULL) || bytesRead == 0) {
            pipe->done = TRUE;
            break;
        }
        pipe->bytes = bytesRead;
        pipe->ready = TRUE;
        offset += bytesRead;
    }
    pipe->done = TRUE;
    return 0;
}

DWORD WINAPI WriterThread(LPVOID param) {
    PIPELINE* pipe = (PIPELINE*)param;
    DWORD bytesWritten;
    clock_t start = clock();

    while (!pipe->done || pipe->ready) {
        if (pipe->ready) {
            WriteFile(pipe->hImage, pipe->buffer, pipe->bytes, &bytesWritten, NULL);
            pipe->totalWritten += bytesWritten;
            pipe->ready = FALSE;

            clock_t now = clock();
            double elapsedSec = (double)(now - start) / CLOCKS_PER_SEC;
            double mbWritten = pipe->totalWritten / (1024.0 * 1024.0);
            double speed = mbWritten / elapsedSec;
            double progress = (double)pipe->totalWritten / pipe->totalSize;
            double eta = (pipe->totalSize - pipe->totalWritten) / (speed * 1024 * 1024);
            draw_progress(progress, speed, eta);
        }
        else Sleep(1);
    }
    return 0;
}

void process_disk(const char* diskPath, const char* imagePath, BOOL isBackup) {
    if (!isBackup) {
        if (_access(imagePath, 0) != 0) {
            printf("이미지 파일 '%s' 를 찾을 수 없습니다.\n", imagePath);
            return;
        }
    }

    HANDLE hDisk = CreateFileA(diskPath, isBackup ? GENERIC_READ : GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hDisk == INVALID_HANDLE_VALUE) {
        printf("디스크 열기 실패 (%lu)\n", GetLastError());
        return;
    }

    HANDLE hImage = CreateFileA(imagePath, isBackup ? GENERIC_WRITE : GENERIC_READ,
        0, NULL, isBackup ? CREATE_ALWAYS : OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hImage == INVALID_HANDLE_VALUE) {
        printf("이미지 파일 열기 실패 (%lu)\n", GetLastError());
        CloseHandle(hDisk);
        return;
    }

    LONGLONG size = isBackup ? get_disk_size(diskPath) : 0;
    if (!isBackup) {
        LARGE_INTEGER sz;
        GetFileSizeEx(hImage, &sz);
        size = sz.QuadPart;
    }

    if (size <= 0) {
        printf("크기를 가져올 수 없습니다.\n");
        CloseHandle(hDisk);
        CloseHandle(hImage);
        return;
    }

    printf("%s 시작 (%.2f GB)\n", isBackup ? "백업" : "복원", size / (1024.0 * 1024 * 1024));

    PIPELINE pipe = { 0 };
    pipe.buffer = (BYTE*)_aligned_malloc(BUFFER_SIZE, 4096);
    pipe.hDisk = hDisk;
    pipe.hImage = hImage;
    pipe.totalSize = size;
    pipe.ready = FALSE;
    pipe.done = FALSE;
    pipe.totalWritten = 0;

    HANDLE hReadThread = CreateThread(NULL, 0, ReaderThread, &pipe, 0, NULL);
    HANDLE hWriteThread = CreateThread(NULL, 0, WriterThread, &pipe, 0, NULL);

    WaitForSingleObject(hReadThread, INFINITE);
    WaitForSingleObject(hWriteThread, INFINITE);

    printf("\n완료: %s\n", imagePath);

    _aligned_free(pipe.buffer);
    CloseHandle(hDisk);
    CloseHandle(hImage);
}

int main() {
    printf("> L A G O (Like Ghost)\n\n");
    printf("[1] 디스크 → 이미지 (이미지 백업)\n");
    printf("[2] 이미지 → 디스크 (이미지 복원)\n");
    printf("선택 (1/2): ");

    int mode; scanf("%d", &mode); getchar();

    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *lastSlash = '\0';
    char imagePath[MAX_PATH];
    snprintf(imagePath, sizeof(imagePath), "%s\\backup.img", exePath);

    DiskInfo disks[32];
    int count = enum_disks(disks, 32);
    if (count == 0) {
        printf("디스크를 찾을 수 없습니다.\n");
        return 1;
    }

    print_disks(disks, count);
    printf("디스크 선택 (1~%d): ", count);

    int sel;
    scanf("%d", &sel);
    getchar();
    if (sel < 1 || sel > count) {
        printf("잘못된 선택입니다.\n");
        return 1;
    }

    const char* diskPath = disks[sel - 1].path;

    if (mode == 1) process_disk(diskPath, imagePath, TRUE);
    else if (mode == 2) {
        printf("\n'%s' 내용이 '%s'에 덮어씌워집니다!\n", imagePath, diskPath);
        printf("계속할까요? (y/N): ");
        char c = getchar();
        if (c == 'y' || c == 'Y') process_disk(diskPath, imagePath, FALSE);
        else printf("복원 취소됨.\n");
    }
    else printf("잘못된 선택입니다.\n");

    return 0;
}