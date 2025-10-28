#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define BUFFER_SIZE (256 * 1024 * 1024) // 256MB x 2
#define PROGRESS_BAR_WIDTH 50

typedef struct {
    int index;
    char path[64];
    char model[128];
    LONGLONG size;
} DiskInfo;

typedef struct {
    BYTE* buffer[2];
    DWORD bytes[2];
    volatile BOOL ready[2];
    volatile BOOL done;
    HANDLE hDisk;
    HANDLE hImage;
    LONGLONG totalSize;
    volatile LONGLONG totalWritten;
} PIPELINE;

int get_system_drive_number() {
    HANDLE hVol = CreateFileA("\\\\.\\C:", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (hVol == INVALID_HANDLE_VALUE) return -1;

    BYTE buffer[1024];
    DWORD bytesReturned;
    if (!DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
        NULL, 0, buffer, sizeof(buffer), &bytesReturned, NULL)) {
        CloseHandle(hVol);
        return -1;
    }

    VOLUME_DISK_EXTENTS* ext = (VOLUME_DISK_EXTENTS*)buffer;
    int diskNumber = (int)ext->Extents[0].DiskNumber;
    CloseHandle(hVol);
    return diskNumber;
}

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
    int idx = 0;

    while (offset < pipe->totalSize) {
        while (pipe->ready[idx]) Sleep(1);

        if (!ReadFile(pipe->hDisk, pipe->buffer[idx], BUFFER_SIZE, &bytesRead, NULL) || bytesRead == 0) {
            pipe->done = TRUE;
            break;
        }

        pipe->bytes[idx] = bytesRead;
        pipe->ready[idx] = TRUE;
        offset += bytesRead;

        idx = 1 - idx;
    }

    pipe->done = TRUE;
    return 0;
}

DWORD WINAPI WriterThread(LPVOID param) {
    PIPELINE* pipe = (PIPELINE*)param;
    DWORD bytesWritten;
    clock_t start = clock();
    int idx = 0;

    while (!pipe->done || pipe->ready[0] || pipe->ready[1]) {
        if (pipe->ready[idx]) {
            BOOL allZero = TRUE;

            for (DWORD i = 0; i < pipe->bytes[idx]; i++) {
                if (pipe->buffer[idx][i] != 0) { allZero = FALSE; break; }
            }

            if (allZero) {
                FILE_ZERO_DATA_INFORMATION zeroData;
                zeroData.FileOffset.QuadPart = pipe->totalWritten;
                zeroData.BeyondFinalZero.QuadPart = pipe->totalWritten + pipe->bytes[idx];

                DWORD tmp;
                DeviceIoControl(pipe->hImage, FSCTL_SET_ZERO_DATA,
                    &zeroData, sizeof(zeroData), NULL, 0, &tmp, NULL);
            }
            else {
                WriteFile(pipe->hImage, pipe->buffer[idx], pipe->bytes[idx], &bytesWritten, NULL);
            }

            pipe->totalWritten += bytesWritten;
            pipe->ready[idx] = FALSE;

            clock_t now = clock();
            double elapsedSec = (double)(now - start) / CLOCKS_PER_SEC;
            double mbWritten = pipe->totalWritten / (1024.0 * 1024.0);
            double speed = mbWritten / elapsedSec;
            double progress = (double)pipe->totalWritten / pipe->totalSize;
            double eta = (pipe->totalSize - pipe->totalWritten) / (speed * 1024 * 1024);

            int filled = (int)(progress * PROGRESS_BAR_WIDTH);
            printf("\r[");
            for (int i = 0; i < PROGRESS_BAR_WIDTH; i++)
                printf(i < filled ? "#" : " ");
            printf("] %.1f%% %.2f MB/s ETA %.1fs", progress * 100, speed, eta);
            fflush(stdout);
        }
        else {
            Sleep(1);
        }

        idx = 1 - idx;
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

    DWORD tmp;
    DeviceIoControl(hImage, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &tmp, NULL);

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

    LARGE_INTEGER sz;
    if (isBackup)
        DeviceIoControl(hDisk, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &sz, sizeof(sz), NULL, NULL);
    else
        GetFileSizeEx(hImage, &sz);

    LONGLONG totalSize = sz.QuadPart;
    printf("%s 시작 (%.2f GB)\n", isBackup ? "백업" : "복원", size / (1024.0 * 1024 * 1024));

    PIPELINE pipe = { 0 };
    pipe.hDisk = hDisk;
    pipe.hImage = hImage;
    pipe.totalSize = totalSize;
    pipe.done = FALSE;
    pipe.ready[0] = FALSE;
    pipe.ready[1] = FALSE;
    pipe.totalWritten = 0;

    // 두 버퍼 생성
    pipe.buffer[0] = (BYTE*)_aligned_malloc(BUFFER_SIZE, 4096);
    pipe.buffer[1] = (BYTE*)_aligned_malloc(BUFFER_SIZE, 4096);

    HANDLE hReadThread = CreateThread(NULL, 0, ReaderThread, &pipe, 0, NULL);
    HANDLE hWriteThread = CreateThread(NULL, 0, WriterThread, &pipe, 0, NULL);

    WaitForSingleObject(hReadThread, INFINITE);
    WaitForSingleObject(hWriteThread, INFINITE);

    printf("\n완료: %s\n", imagePath);

    _aligned_free(pipe.buffer[0]);
    _aligned_free(pipe.buffer[1]);
    CloseHandle(hDisk);
    CloseHandle(hImage);
}

int main() {
    printf("   __   ___  _________     \n");
    printf("  / /  / _ |/ ___/ __ \\   \n");
    printf(" / /__/ __ / (_ / /_/ /    \n");
    printf("/____/_/ |_\\___/\\____/   \n\n");
    printf(">> L A G O (Like Ghost)\n");
    printf(">> (C) ROKN. KIM SANG YUN.\n\n");
    printf("[1] 디스크 → 이미지 (이미지 백업)\n");
    printf("[2] 이미지 → 디스크 (이미지 복원)\n\n");
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

    DiskInfo* selectedDisk = &disks[sel - 1];
    int systemDriveNum = get_system_drive_number();

    if (systemDriveNum == -1) {
        printf("시스템 드라이브 정보를 가져올 수 없습니다.\n");
    }
    else if (selectedDisk->index == systemDriveNum) {
        printf("선택한 디스크는 시스템 드라이브(C:) 입니다.\n");
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