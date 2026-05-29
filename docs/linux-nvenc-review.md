# Linux zero-copy путь (NVENC + CUDA): обзор, изменения, методика замеров

> **Назначение документа.** Это рабочее задание для отдельной сессии Claude Code. Цель сессии: внести изменения в текущую Linux-реализацию zero-copy стриминга (`UGstH264AppSrcComponent`) и **замерить эффект числами** (CLAUDE.md: «без чисел изменение не считается завершённым»). Документ самодостаточен — контекст обсуждения в нём изложен.
>
> **Соблюдать workflow из CLAUDE.md:** explore → plan → implement. Сначала прочитать указанные файлы, предложить план, согласовать, потом код. Не полагаться на память для API NVENC/CUDA/Vulkan/UE — сверяться с официальной документацией (ссылки в конце). Перед задачей, требующей поиска по docs, прогрепать `docs/research-log.md`.
>
> **Область:** только Linux NVENC+CUDA путь отправки. Приёмный путь (`UGstAppSinkComponent`/`FGstTexture`) и generic copy-путь (`UGstAppSrcComponent`) — вне области, кроме явно указанных мест.

---

## 1. Что это за путь (краткая карта)

Полная архитектура — `docs/architecture.md`, поток данных «по-человечески» — `README.md`. Кратко:

```
USceneCaptureComponent2D (SCS_FinalColorLDR)
  → USharedRenderTarget2D (RTF_RGBA8)
  → compute shader BGRAtoNV12 (RDG) → 2 экспортируемые VkImage: Y (R8), UV (R8G8), tiling OPTIMAL
  → SubmitCommandsHint → vkQueueSubmit({signal: VkSemaphore})
  → CUDA: cudaWaitExternalSemaphoresAsync → cudaMemcpy2DFromArrayAsync ×2 в pitched NV12-буфер
  → (background thread) cudaStreamSynchronize → NVENC EncodeFrame → GstAppSrc::PushBuffer → shmsink
```

Ключевые файлы:
- `Private/GstH264AppSrcComponent.cpp` (765 строк) — основной класс, вложенная структура `FVulkanExportResources` (строка ~45).
- `Public/GstH264AppSrcComponent.h` — UPROPERTY, дефолты.
- `Private/NvEncEncoder.cpp` / `Public/NvEncEncoder.h` — обёртка NVENC.
- `Private/BGRAtoNV12Pass.cpp` / `Public/BGRAtoNV12Pass.h` — диспатч compute-шейдера.
- Шейдер `BGRAtoNV12.usf` (регистрируется как `/Project/Private/BGRAtoNV12.usf` в `BGRAtoNV12Pass.cpp:3`) — **в репозитории пока отсутствует, будет скопирован**. Без него часть изменений (раздел 3.1) не реализовать — проверить наличие до старта.

Платформенные ограничения: NVENC принимает вход только как CUDA / OpenGL / DirectX — **прямого Vulkan-входа у NVENC нет**, поэтому CUDA здесь обязательный мост, а не избыточность. Полностью убрать CUDA можно только сменой энкодера (Vulkan Video / VA-API) — это вне области данного документа.

---

## 2. Проблемы текущей реализации

Серьёзность: **High** = вероятный баг/утечка/краш, **Med** = риск при условиях, **Low** = чистота. Полный аудит всего модуля — `docs/code-audit.md`; ниже только то, что относится к этому пути.

### Производительность (главная причина изменений)

- **[High-perf] Лишняя полнокадровая GPU→GPU копия.** Шейдер пишет в tiled VkImage (Y/UV), затем CUDA копирует их в **отдельный** pitched NV12-буфер (`cudaMemcpy2DFromArrayAsync` ×2, `GstH264AppSrcComponent.cpp:661` и `:666`) каждый кадр. NVENC читает уже из буфера-копии. Копия нужна только потому, что шейдер пишет в картинки, а NVENC хочет линейный NV12. Устранимо — см. 3.1.
- **[Med-perf] Нет пайплайнинга кадров.** `std::atomic<bool> bEncoding` (`:169`, проверка `:554`, установка `:568`) разрешает один кадр в полёте. Захват N+1 не начинается, пока encode N не завершён. Один NV12-буфер и один bitstream-буфер NVENC не дают перекрывать convert/encode. Ограничивает throughput и в меньшей степени latency.

### Корректность

- **[High] `FNvEncEncoder::Flush` не возвращает NAL** (`NvEncEncoder.cpp:201-210`). Шлёт EOS-кадр, но не делает `nvEncLockBitstream` — `OutNAL` всегда пуст, хвостовые кадры при завершении теряются (`GstH264AppSrcComponent.cpp:195-200` проверяет `Num()>0`). С текущим пресетом (P4 ULL, без B-кадров) эффект, скорее всего, нулевой, но контракт сломан.
- **[Med] Неверное лог-сообщение** (`GstH264AppSrcComponent.cpp:329`): «→ udp://127.0.0.1:5004», хотя пишется в `shmsink`. Исправить на фактический `ShmSocketPath`.
- **[Med] Коды возврата CUDA игнорируются массово**: `cudaStreamCreate` (`:154`), `cudaGetMipmappedArrayLevel` (`:110`,`:140`), `cuCtxSetCurrent` (`:597`,`:653`,`:674`), `cudaWaitExternalSemaphoresAsync` (`:658`), оба `cudaMemcpy2DFromArrayAsync` (`:661`,`:666`), `cudaStreamSynchronize` (`:676`). Ошибка ведёт к мусорному кадру без диагностики. Добавить проверку и лог.

### Утечки / жизненный цикл

- **[High] Двойной Destroy + Vulkan вне render thread.** Деструктор `~FVulkanExportResources` (`:257`) вызывает `DestroyOnGameThread(); DestroyOnRenderThread();`. Но `EndPlay` (`:756-761`) уже вызвал оба (render — через render-команду + `FlushRenderingCommands`). При отпускании `TSharedPtr` деструктор повторяет оба, и `DestroyOnRenderThread` (vkDestroyImage/vkFreeMemory/vkDestroySemaphore) исполняется не на render thread. Сделать destroy однократным (флаги/guard), не дублировать в деструкторе.
- **[High] Утечка file descriptors при сбое импорта.** `MemFdY`/`MemFdUV`/`SemaphoreFd` обнуляются только **после успешного** `cudaImportExternal*` (`:98`,`:128`,`:152`). При провале импорта или если `bCudaReady` не достигнут — fd не закрывается нигде (ни в `DestroyOnGameThread`, ни в `DestroyOnRenderThread` нет `close()`). Добавить `close()` для всех оставшихся ≥0 fd в teardown.
- **[Med] Частичный провал `CreateExternalImage`/`ImportCudaResources`** оставляет уже созданные ресурсы до общего Destroy (`:408-513`, `:78-159`). В сочетании с двумя пунктами выше повышает риск. При рефакторинге под 3.1 эти пути упростятся.

### Thread-safety

- **[Med] `StreamFrameIdx` без атомарности.** Читается на render thread как `DbgFrame` (`:602`), инкрементируется на background thread (`:727`). Эффект — кривые числа в логах (рядом стоящие счётчики уже атомарные). Сделать атомарным или убрать гонку.

### Прочее

- **[Low] `GST_ENABLE_DEBUG_LOG` всегда включён** (`Public/Shared.h:6`), а `StartupModule` ставит `EGstVerbosity::Debug` (`GStreamerModule.cpp:24`). Для замеров это удобно (см. раздел 4), но в релизе шумно — не забыть про это при финализации (не в этой сессии).

---

## 3. Предлагаемые изменения

> Это направление, а не готовый дизайн. На этапе plan проработать открытые вопросы (отмечены **OPEN**) по официальной документации и согласовать, прежде чем писать код.

### 3.1 [Приоритет 1] Убрать лишнюю копию: шейдер пишет прямо в линейный экспортируемый VkBuffer

**Идея.** Вместо двух tiled VkImage + cudaArray + двух `cudaMemcpy2D` — один экспортируемый **линейный VkBuffer** в раскладке NV12 (Y-плоскость `pitch*H`, затем UV-плоскость `pitch*(H/2)`). Шейдер пишет в него напрямую. CUDA импортирует буфер как обычный device ptr (`cudaExternalMemoryGetMappedBuffer`). NVENC регистрирует этот ptr (как сейчас, но указывает на импортированный буфер вместо отдельного `cudaMallocPitch`).

**Что уходит:**
- два `cudaMemcpy2DFromArrayAsync` на кадр (`:661`,`:666`);
- cudaArray / `cudaMipmappedArray` импорты и `cudaExternalMemoryGetMappedMipmappedArray` (`:101-140`);
- отдельный `cudaMallocPitch` буфер (`:308`);
- два UAV-текстуры NV12 заменяются на один UAV-буфер.

**Ожидаемый эффект:** минус один полнокадровый device-to-device копир на кадр (должно сократиться время между сигналом семафора и завершением `cudaStreamSynchronize`), меньше пиковой GPU-памяти, меньше кода в `ImportCudaResources`.

**OPEN-вопросы для этапа plan (сверить с docs, не по памяти):**
1. **Выравнивание pitch для NVENC.** NVENC при регистрации `CUDADEVICEPTR` с `NV_ENC_BUFFER_FORMAT_NV12` требует определённого выравнивания input pitch. Узнать требование из NVENC Programming Guide; раньше pitch брался от `cudaMallocPitch` (естественно выровнен). При ручном VkBuffer pitch выбираем сами — выровнять корректно (кандидат: 256/512, **подтвердить**). Неверный pitch → артефакты/ошибка кодера.
2. **Запись байтов из compute в RWByteAddressBuffer.** Y — 1 байт/пиксель, UV — 2 байта/пиксель (interleaved). `ByteAddressBuffer.Store` пишет 32-битными словами → во избежание read-modify-write гонок проектировать так, чтобы каждый тред писал выровненные слова (например, 4 горизонтальных Y-сэмпла = одно слово; для UV — пара (U,V) ×2 = слово). Это основная сложность изменения — продумать раскладку тредов 16×16 и pitch.
3. **Создание экспортируемого VkBuffer и его регистрация в RDG.** Сейчас для картинок используется `RHICreateTexture2DFromResource` (`:501`) + `RegisterExternalTexture`. Для буфера нужен аналог: создать `VkBuffer` с `VkExternalMemoryBufferCreateInfo` (handleType OPAQUE_FD), usage `STORAGE_BUFFER|TRANSFER_SRC`, экспортировать fd, и зарегистрировать как `FRDGBufferRef` (`RegisterExternalBuffer`) для UAV в шейдере. Проверить доступные хелперы `IVulkanDynamicRHI` для buffer-from-resource; если их нет — создавать `FRHIBuffer` обёртку вручную. **Это исследовательский пункт.**

   > **БЛОКЕР (исследовано 2026-05-29, см. `docs/research-log.md`).** В UE 5.7.2 экспортируемая память реализована только для текстур (`TexCreate_External`), у Vulkan-буферов её нет, и `RHICreateBufferFromResource` в `IVulkanDynamicRHI` отсутствует. Получить экспортируемый VkBuffer под UE-шейдер штатно нельзя. Жизнеспособные направления: **A** — один экспортируемый LINEAR VkImage R8 W×(H+H/2), CUDA маппит как буфер (риск: STORAGE на LINEAR может быть не поддержан на NVIDIA, нужна runtime-проверка); **B** — собственный raw-Vulkan compute-dispatch в экспортируемый VkBuffer мимо UE-биндинга (объёмно); **C** — не убирать копию, а перекрывать пайплайнингом (3.3). Выбор направления — за владельцем.
4. **Раскладка/strides в шейдере vs то, что ждёт NVENC.** UV-плоскость в NV12 — половинное разрешение, interleaved U,V. Убедиться, что offset UV-плоскости = `pitch*H` и stride = `pitch`.

Если 3.1 окажется слишком объёмным для одной сессии — допустимо сделать сначала раздел 3.2 (дешёвые фиксы) и замерить базовую линию, а 3.1 отдельным заходом.

### 3.2 [Приоритет 2] Корректность и жизненный цикл (независимо от 3.1, низкий риск)

Сделать в любом случае:
- Однократный Destroy (устранить двойной вызов; `:257` vs `:756`).
- `close()` для оставшихся fd в teardown (`MemFdY`/`MemFdUV`/`SemaphoreFd`).
- Проверки кодов возврата CUDA + лог при ошибке.
- Исправить лог-сообщение `:329`.
- `StreamFrameIdx` → атомарный (или убрать чтение на render thread).
- `Flush`: либо реализовать lock+copy bitstream, либо честно задокументировать, что с текущим пресетом дренаж не нужен, и убрать вводящий в заблуждение контракт в `NvEncEncoder.h:31-32`.

### 3.3 [Опционально, обсудить] Пайплайнинг

Двойная буферизация NV12 + несколько bitstream-буферов NVENC, чтобы перекрывать convert(N+1) и encode(N). Повышает throughput, усложняет синхронизацию и `bEncoding`-логику. **Не делать вместе с 3.1** — сначала измерить эффект 3.1 в одиночку, иначе нельзя будет атрибутировать выигрыш. Кандидат на отдельную итерацию.

### Не трогать (будущее направление)

Планируется работа с **DirectX 12** (в дополнение к Vulkan) и, возможно, Vulkan Video. Поэтому: не выпиливать абстракции `IGstPipeline/IGstAppSrc` и не «зашивать» CUDA-специфику глубже в общий код — наоборот, при рефакторинге 3.1 держать CUDA-interop локализованным в `FVulkanExportResources`, чтобы платформенные бэкенды (DX12 shared handle) можно было добавить рядом. Сам DX12-путь — вне этой сессии.

---

## 4. Методика замеров (обязательна до и после)

Принцип: **A/B по числам на одинаковой сцене**. «Стало быстрее» без цифр не принимается.

### 4.1 Что уже есть в коде для измерений

Использовать имеющуюся инструментацию, не изобретать заново:
- **Атомарные счётчики глубины конвейера** (`:171-175`): `SubmitEnqueued/SubmitCompleted/CudaSynced/Encoded/Pushed`. Heartbeat каждые 60 кадров (`:602-619`) логирует `submit_pending`, `cuda_pending` — индикаторы сталла.
- **Потактовые тайминги** (логируются при превышении порогов): `vkQueueSubmit` ms (`:640`), `cudaStreamSynchronize` ms (`:680`), `EncodeFrame` ms (`:691`), `PushBuffer` ms (`:722`). Полная строка каждые 300 кадров (`:728-732`): `sync=..ms enc=..ms push=..ms nal=..`.
- **SEI-таймстамп в потоке** (`:698-719`): в H.264 встраивается UUID + `push_ts` (ns, CLOCK_MONOTONIC) + `sync_us` + `enc_us`. Это даёт приёмнику посчитать **glass-to-glass латентность**, если он засекает время на декоде/выводе и вычитает `push_ts`.

Включить подробный лог: `GST_ENABLE_DEBUG_LOG` уже определён, verbosity по умолчанию Debug. Для чистых замеров лучше **дописать CSV-дамп** per-frame (sync_us, enc_us, push_us, submit_pending, cuda_pending, nal_size) в файл — парсить логи хрупко. Это допустимое временное изменение для бенчмарка (пометить «remove after benchmark»).

### 4.2 Метрики

Главная для изменения 3.1 — **стоимость convert+copy на GPU**, т.е. время от сигнала семафора до завершения `cudaStreamSynchronize` (в коде — `sync_us`). Убираем копию → `sync_us` должен упасть.

Снимать:
1. **Per-stage время** (mean / p50 / p95 / p99): `sync_us` (ожидание GPU: convert + копия), `enc_us` (NVENC), `push_us` (GStreamer push).
2. **End-to-end латентность** glass-to-glass через SEI `push_ts` (нужен приёмник-харнесс, см. 4.4).
3. **Throughput**: устойчивый fps при `bCaptureEveryFrame=true` и снятом ограничении `TargetFPS` (или поднять `TargetFPS` выше предельного и смотреть фактический).
4. **Глубина конвейера**: `submit_pending`, `cuda_pending` (растущий backlog = сталл).
5. **Пиковая GPU-память** процесса (3.1 должна снизить — нет отдельного NV12-буфера + cudaArray).
6. **NVENC util** (отдельный аппаратный блок) и **GPU util**.
7. **Точность битрейта**: фактический средний битрейт vs заданный `Bitrate` (по размеру выхода за время).
8. **Стабильность fd**: число открытых fd процесса до/во время/после (для проверки фикса утечки 2.1).

### 4.3 Инструменты

- `nvidia-smi dmon -s um` — GPU util, mem, **encoder util (`%enc`)** в реальном времени.
- `nvidia-smi --query-gpu=memory.used --format=csv -l 1` — память.
- **Nsight Systems** (`nsys profile`) — таймлайн GPU: явно покажет `cudaMemcpy2D` до и его отсутствие после. Лучшее доказательство, что копия ушла.
- `ls /proc/<pid>/fd | wc -l` в цикле — счётчик fd (утечка).
- `GST_DEBUG=3` (или выше для appsrc/shmsink) — диагностика pipeline.
- Размер выхода: считать суммарные байты NAL за фиксированное число кадров → средний битрейт.

### 4.4 Протокол A/B

1. **Фиксировать сцену и параметры.** Одна и та же сцена (детерминированная камера/анимация, без случайностей), фиксированные `Width/Height`, `Bitrate`, `MaxBitrate`, `GopLength`, `TargetFPS`. Прогнать два разрешения: **1920×1080** и **3840×2160** (на 4K эффект копии заметнее).
2. **Warmup.** Отбросить первые ~300 кадров (инициализация CUDA-импорта ленивая — `:595-600`, первые кадры аномальны).
3. **Длина прогона.** ≥ 2000 кадров на конфигурацию для стабильных перцентилей; для проверки отсутствия утечек/деградации — отдельный длинный прогон ≥ 10000 кадров со снятием fd-счётчика и `cuda_pending`.
4. **Снять baseline** (текущий код) → зафиксировать все метрики из 4.2.
5. **Внести изменение** (3.2, затем 3.1) → снять те же метрики на той же сцене.
6. **A/B-переключение.** Желательно иметь возможность собрать обе версии (или флаг компиляции/ветку), чтобы прогон был на идентичной сцене. Если флага нет — гонять последовательно на одной и той же детерминированной сцене.

### 4.5 Критерии приёмки изменения 3.1

- `sync_us` (mean и p95) **снизился** — основной ожидаемый эффект (копия ушла). На 4K снижение должно быть заметнее.
- End-to-end латентность (p95) **не хуже** baseline.
- Пиковая GPU-память процесса **не выше** (ожидается ниже).
- NVENC `%enc` и качество (битрейт при заданном target) **без регресса**; визуально картинка идентична (цвет/гамма не поехали — критично, т.к. меняется путь записи NV12).
- Нет новых CUDA/Vulkan ошибок в логе за длинный прогон.
- Счётчик fd **стабилен** (фикс 2.1), `cuda_pending`/`submit_pending` не растут (нет сталла).

Если `sync_us` не упал — значит, узкое место не в копии (например, доминирует ожидание `vkQueueSubmit`/семафора); тогда зафиксировать это числами и пересмотреть гипотезу, а не «чинить» вслепую.

---

## 5. Чеклист для сессии

1. Прочитать: `Private/GstH264AppSrcComponent.cpp`, `Private/NvEncEncoder.cpp`, `Private/BGRAtoNV12Pass.cpp`, шейдер `BGRAtoNV12.usf` (проверить, что скопирован), `docs/architecture.md`, `docs/code-audit.md`. Прогрепать `docs/research-log.md`.
2. Снять **baseline** по разделу 4 (до любых правок).
3. Сделать 3.2 (дешёвые фиксы), пересобрать, прогнать — убедиться, что ничего не сломалось.
4. Согласовать план по 3.1 (OPEN-вопросы 1–4), затем реализовать.
5. Снять метрики после, сравнить с baseline по критериям 4.5.
6. Записать находки по API (pitch alignment, external VkBuffer в UE RHI и т.п.) в `docs/research-log.md`.
7. Сборку проверять не только project-build, но и через `BuildPlugin`, если затронут `Build.cs`/ThirdParty (CLAUDE.md).

---

## Источники (сверять при реализации, указывать версии)

- NVENC Video Encoder API Programming Guide — https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html (input resource types, pitch/alignment для NV12 CUDADEVICEPTR).
- CUDA Runtime API, External Resource Interoperability — `cudaImportExternalMemory`, `cudaExternalMemoryGetMappedBuffer` (для импорта VkBuffer как device ptr вместо cudaArray).
- UE 5.7 docs — https://dev.epicgames.com/documentation/en-us/unreal-engine (RDG buffers: `RegisterExternalBuffer`, UAV на буфер; `IVulkanDynamicRHI`).
- Vulkan spec — `VkExternalMemoryBufferCreateInfo`, `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT`, `VkExportMemoryAllocateInfo` (создание экспортируемого VkBuffer).
