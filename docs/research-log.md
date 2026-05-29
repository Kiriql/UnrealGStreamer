# Research log — GStreamerModule zero-copy путь

> Лог находок по API (UE RHI / Vulkan / CUDA / NVENC), которые неочевидны и стоили времени. Перед поиском в вебе — грепать этот файл. Каждая запись: дата, вопрос, факт, источник (файл движка / версия доки).

---

## 2026-05-29 — Экспортируемая память в UE 5.7.2 Vulkan RHI: текстуры да, буферы нет

**Вопрос (OPEN-3 из linux-nvenc-review.md).** Можно ли создать экспортируемый (OPAQUE_FD) линейный VkBuffer, в который пишет UE compute-шейдер (через RDG UAV), и импортировать его в CUDA как device ptr — чтобы убрать `cudaMemcpy2DFromArray` ×2?

**Факт: нельзя через публичный API.**

- `IVulkanDynamicRHI` (UE5.7.2 `Engine/Source/Runtime/VulkanRHI/Public/IVulkanDynamicRHI.h`): есть `RHICreateTexture2DFromResource(... VkImage ...)`, но **нет** аналога `...BufferFromResource`. Обернуть внешний VkBuffer в `FRHIBuffer`/`FRDGBuffer` для биндинга в шейдере штатно нечем. Есть только обратное `RHIGetAllocationInfo(FRHIBuffer*) -> {VkDeviceMemory, Offset, Size}`.
- Память UE-буферов суб-аллоцируется из пулов (offset != 0) и **не** создаётся экспортируемой: `VulkanBuffer.cpp` не выставляет `VkExternalMemoryBufferCreateInfo` и не ставит флаг `EVulkanAllocationFlags::External`. Флаг `BUF_Shared` (`RHIDefinitions.h:992`) для Vulkan-буфера ни во что не транслируется.
- Экспортируемая память реализована **только для текстур**: `VulkanTexture.cpp:379` — при `TexCreate_External` добавляется `VkExternalMemoryImageCreateInfo` (OPAQUE_FD), аллокация форсируется отдельной (dedicated). Сам экспорт fd в движке наружу не отдаётся — fd надо тянуть самим через `vkGetMemoryFdKHR` (как уже делает `GstH264AppSrcComponent.cpp`).

**Следствие для дизайна.** Истинный zero-copy (без `cudaMemcpy`) требует одной экспортируемой линейной аллокации в раскладке NV12 (Y: pitch*H, затем UV: pitch*H/2), в которую пишет шейдер. Варианты:
- **A. Один экспортируемый LINEAR VkImage R8 размера W×(H+H/2)** (Y в строках [0,H), UV-байты в [H,H+H/2)), CUDA маппит его dedicated-память как буфер (`cudaExternalMemoryGetMappedBuffer`), NVENC регистрирует ptr+rowPitch. Обёртка `RHICreateTexture2DFromResource` доступна. **Риск:** STORAGE на LINEAR-тайлинге для R8 часто не входит в `linearTilingFeatures` на NVIDIA — нужна runtime-проверка `vkGetPhysicalDeviceFormatProperties(...).linearTilingFeatures & STORAGE_IMAGE_BIT`; при отсутствии — путь невозможен.
- **B. Свой raw-Vulkan compute-dispatch** (собственный pipeline/descriptor set/SPIR-V) в экспортируемый VkBuffer, минуя UE-биндинг шейдера. Тяжело по объёму и поддержке.
- **C. Не убирать копию**, а перекрывать её с encode (пайплайнинг, раздел 3.3 review-доки).
- **D. Модификация движка** (добавить buffer-from-resource / экспортируемый буфер) — вне правил (ThirdParty/движок не трогаем).

## 2026-05-29 — NVENC input pitch alignment для CUDADEVICEPTR NV12

**Вопрос (OPEN-1).** Требование к выравниванию input pitch при `nvEncRegisterResource(NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR, NV_ENC_BUFFER_FORMAT_NV12)`.

**Факт.** Явного числа в NVENC Programming Guide (v13.0) нет. Гайд лишь требует буферы из семейства `cuMemAlloc*`. NVIDIA-сэмплы (NvEncoder) и FFmpeg используют `cuMemAllocPitch`, который даёт естественно выровненный pitch (на практике 512 Б), и передают этот pitch как есть. Текущий код через `cudaMallocPitch` это и обеспечивает. UV-плоскость NVENC ожидает по offset = pitch*Height в той же аллокации. **Вывод:** при ручном выборе pitch выравнивать ≥256–512 Б; источник чисел — не нашёлся, держать `cudaMallocPitch`-совместимый pitch.

Источник: NVENC Video Encoder API Programming Guide 13.0 (явных alignment-значений не содержит); подтверждение паттерна — NVIDIA NvEncoder samples, FFmpeg `libavcodec/nvenc.c`.

## 2026-05-30 — Конфликт имени `GError` (UE vs glib) и canonical fix

**Контекст**: подключение GStreamer 1.28.3 в Win64-плагин `UnrealGStreamer`. При `#include <glib.h>`/`<gst/gst.h>` в одном TU с UE-инклудами компиляция падает в массиве glib-хедеров (`gconvert.h`, `gdir.h`, `gfileutils.h`, `gunicode.h`, `giochannel.h`) — `error C2061: GError` / `C2378: переопределение; символ нельзя перегрузить typedef`.

**Источник**: `Engine/Source/Runtime/Core/Public/CoreGlobals.h:99` объявляет `CORE_API extern class FOutputDeviceError* GError;`. В glib 2.84 (часть GStreamer 1.28) `glib/gerror.h:43` определяет `typedef struct _GError GError;`. Имя `GError` зарезервировано на обеих сторонах, MSVC с `/permissive-` в UE отказывается мержить «extern переменная» и «typedef имени тип».

**Вывод**: standalone `cl.exe` с теми же include-путями компилирует glib чисто — проблема исключительно в UE-окружении (CoreGlobals.h в TU). Лечится не workaround'ом `#define GError ...`, а **разделением TU**: один `.cpp` под UE (без gst/glib), второй `.cpp` под gst/glib (без `CoreMinimal.h`/CoreGlobals), мост между ними — узкий C-style header с plain-типами (`uint32_t`, `const char*`, никаких `GError`/`FString`/UE-макросов). Это паттерн Linux-модуля (`GstCoreImpl.cpp`).

**Применение**: `Plugins/UnrealGStreamer/Source/GStreamer/Private/Core/GstCore.{h,cpp}` — gst-сторона; `Private/Core/GStreamerModule.cpp` — UE-сторона. Build.cs: `PCHUsage = NoPCHs; bUseUnity = false;` (без `bUseUnity=false` unity-агрегация снова слепит UE+gst в один TU).
