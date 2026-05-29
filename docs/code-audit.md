# Аудит кода GStreamerModule

> Дата: 2026-05-28. Снимок текущего состояния. Это список наблюдений, не план рефакторинга — решение что чинить и в каком порядке за владельцем. Серьёзность субъективна (High = вероятный баг/утечка/краш, Med = риск при определённых условиях, Low = чистота/мелочь).
>
> Прочитан весь модуль (`Public/*.h`, `Private/*.cpp`, `GStreamerModule.Build.cs`, `README.md`). Шейдер `BGRAtoNV12.usf` пока не скопирован в репозиторий — не аудирован. Отсутствие `.uplugin` и шейдера — ожидаемое переходное состояние (вынос в отдельный плагин запланирован), в аудит как дефекты не включено.

---

## 1. Корректность и потенциальные баги

### 1.1 [High] `FNvEncEncoder::Flush` никогда не возвращает NAL
`Private/NvEncEncoder.cpp:201-210`. Отправляет EOS-кадр (`NV_ENC_PIC_FLAG_EOS`) через `nvEncEncodePicture`, но не вызывает `nvEncLockBitstream`/`LockAndCopyBitstream` — `OutNAL` всегда остаётся пустым. Контракт в `NvEncEncoder.h:31-32` («drain remaining frames into OutNAL») не выполняется. В `GstH264AppSrcComponent.cpp:195-200` результат проверяется `FlushNAL.Num() > 0`, поэтому хвостовые кадры при завершении теряются молча. С текущим пресетом (P4 ULL, `frameIntervalP=1`, без B-кадров) буферизации скорее всего нет, и эффект нулевой — но код сломан и опасен при смене пресета.

### 1.2 [High] Двойной вызов Destroy в `FVulkanExportResources`
`GstH264AppSrcComponent.cpp:257` (деструктор) вызывает `DestroyOnGameThread(); DestroyOnRenderThread();`. Но в `EndPlay` (`:756-761`) уже вызывается `DestroyOnGameThread()` явно и `DestroyOnRenderThread()` через render-команду + `FlushRenderingCommands`. Когда `TSharedPtr` затем отпускается, деструктор вызывает оба destroy **повторно** и `DestroyOnRenderThread` исполняется **не на render thread** (Vulkan-вызовы `vkDestroyImage`/`vkFreeMemory`/`vkDestroySemaphore` вне render thread). `DestroyOnGameThread` частично идемпотентен (проверки на nullptr), но повторный путь Vulkan — реальный риск. Стоит сделать destroy строго однократным и не дублировать в деструкторе.

### 1.3 [High] Создание `UTexture2D` на render thread
`GstTexture.cpp:164-192` `RenderCmd_CreateTexture` вызывает `UTexture2D::CreateTransient` + `AddToRoot` + `UpdateResource` внутри `ENQUEUE_RENDER_COMMAND` (`:157`). Создание UObject и манипуляции GC-root должны выполняться на game thread. На render thread это гонка с GC и потенциальный краш. (Сам `RenderCmd_UpdateTexture` на render thread — корректно.)

### 1.4 [Med] Неверное лог-сообщение
`GstH264AppSrcComponent.cpp:329`: лог «GStreamer H264 pipeline started → udp://127.0.0.1:5004», тогда как pipeline по умолчанию пишет в `shmsink` (`/tmp/ue_h264`). Вводит в заблуждение при диагностике.

### 1.5 [Med] BUS ERROR не парсит GError
`GstPipelineImpl.cpp:186-189`: на `GST_MESSAGE_ERROR` логируется только имя pipeline, без `gst_message_parse_error()` — теряется текст ошибки и debug-инфо, что критично для диагностики падений pipeline. `GST_MESSAGE_WARNING` (`:191`) тоже без парсинга.

### 1.6 [Med] EOS делает seek на live-pipeline
`GstPipelineImpl.cpp:179-184`: на `GST_MESSAGE_EOS` выполняется `gst_element_seek` (рестарт с 0). Для live appsrc-стрима это странное поведение; назначение неочевидно и не прокомментировано.

### 1.7 [Med] Массовое игнорирование кодов возврата CUDA
`GstH264AppSrcComponent.cpp`: `cudaStreamCreate` (`:154`), `cudaGetMipmappedArrayLevel` (`:110`, `:140`), `cuCtxSetCurrent` (`:597`, `:653`, `:674`), `cudaWaitExternalSemaphoresAsync` (`:658`), оба `cudaMemcpy2DFromArrayAsync` (`:661`, `:666`), `cudaStreamSynchronize` (`:676`) — возврат не проверяется. Ошибка CUDA не диагностируется и тихо ведёт к мусорному/пустому кадру.

### 1.8 [Low] Рассинхрон значения по умолчанию `GopLength`
`GstH264AppSrcComponent.h:31` default `GopLength = 25`; `NvEncEncoder.h:15` `FNvEncConfig::GopLength = 60`; README (строка 68) пишет 60. Не баг (компонент задаёт явно), но источник путаницы.

### 1.9 [Low] `FGstAppSinkImpl::OnNewSample` фиксирует format/width/height один раз
`GstAppSinkImpl.cpp:119-129`: caps читаются только пока `m_Format == UNKNOWN`. Если caps меняются в рантайме (renegotiation), новые размеры игнорируются. Для динамического разрешения — баг; для фиксированного — ок.

### 1.10 [Low] `gst_app_src_push_buffer` без проверки `gst_buffer_new_wrapped_full`
`GstAppSrcImpl.cpp:92`: возврат `gst_buffer_new_wrapped_full` не проверяется на null перед `gst_app_src_push_buffer`.

---

## 2. Утечки ресурсов

### 2.1 [High] Утечка file descriptors при неудачном CUDA-импорте
`GstH264AppSrcComponent.cpp`: `MemFdY`/`MemFdUV`/`SemaphoreFd` обнуляются (`= -1`) только **после успешного** `cudaImportExternalMemory`/`cudaImportExternalSemaphore` (`:98`, `:128`, `:152`). Если импорт провалился — fd остаётся открытым и нигде не закрывается (`DestroyOnGameThread`/`DestroyOnRenderThread` не вызывают `close()` ни для одного fd). Аналогично: если `bCudaReady` так и не достигнут (например, поток инициализации прервался после получения fd, но до импорта), три fd текут на весь срок жизни. Нужен `close()` для всех оставшихся ≥0 fd в destroy.

### 2.2 [Med] Частичный провал `ImportCudaResources` не откатывает уже импортированное
`GstH264AppSrcComponent.cpp:78-159`: при провале на UV (после успешного Y) функция возвращает `false`, но уже созданные `CudaExtMemY`/`CudaMipY` не освобождаются здесь. Освобождение произойдёт позже в `DestroyOnGameThread`, **только если** структура доживёт до уничтожения — обычно да, так что это скорее «грязно», но в комбинации с 1.2/2.1 повышает риск.

### 2.3 [Med] Частичный провал `CreateExternalImage` оставляет ресурсы
`GstH264AppSrcComponent.cpp:408-513`: при провале на середине (например, `RHICreateTexture2DFromResource` после успешного `vkAllocateMemory` и получения fd) `OutImage`/`OutMemory` уже созданы и `OutFd` получен. Возврат `false` — а очистка опять отложена на `DestroyOnRenderThread` (Vulkan-объекты освободит, но fd — нет, см. 2.1).

### 2.4 [Low] `cudaMallocPitch` / NV12-буфер при провале pipeline
`GstH264AppSrcComponent.cpp:308`: если NVENC init прошёл, `cudaMallocPitch` успешен, но GStreamer init провалился, `NV12CudaPtr` уже выделен. Освобождается в `DestroyOnGameThread` — ок, но `bStreamReady` остаётся false, а ресурсы висят до EndPlay. Не утечка, но ранний выход без явного освобождения.

### 2.5 [Low] `gst_app_src_end_of_stream` в `Disconnect` всегда
`GstAppSrcImpl.cpp:76`: при каждом `Disconnect` шлётся EOS, даже если pipeline уже в NULL/разрушается. Безвредно в большинстве случаев, но лишнее действие на teardown.

---

## 3. Thread-safety

### 3.1 [High] Гонка на пуле сэмплов `FGstAppSinkImpl::AllocSample`
`GstAppSinkImpl.cpp:164-180`: первая проверка `if (!m_SamplePool.empty())` (`:167`) читается **без удержания `m_SampleMx`**, лишь затем берётся lock. Это data race на `std::vector` (конкурентно с `DeallocSample`/`DeallocSamplePool` под локом). Double-checked locking здесь некорректен для не-атомарного контейнера. `AllocSample` дёргается из `OnNewSample` на стриминг-потоке GStreamer.

### 3.2 [High] Teardown appsink при живом callback
`GstAppSinkComponent.cpp:16-21` `ResetState`: `AppSink->Disconnect()` затем `SafeDestroy(Texture)`/`SafeDestroy(AppSink)`. Между установкой `emit-signals=FALSE`/`unref` в `Disconnect` (`GstAppSinkImpl.cpp:91-100`) и завершением выполняющегося `OnNewSample`/`CbGstSampleReceived` нет барьера. Если sample-callback в полёте на стриминг-потоке, он обращается к `m_Callback`/`Texture`, которые game thread уничтожает — use-after-free. Нужна синхронизация (дождаться дренажа потока перед уничтожением).

### 3.3 [Med] `StreamFrameIdx` без атомарности
`GstH264AppSrcComponent.cpp`: читается на render thread как `DbgFrame` (`:602`) и инкрементируется на background thread (`:727`). Не атомарно. Эффект — только некорректные значения в логах (диагностические счётчики рядом — атомарные), но формально data race.

### 3.4 [Low] `FGstTexture::Release` вызывает `FlushRenderingCommands` из game thread
`GstTexture.cpp:200`: `Release` (через `Resize`) на каждой смене формата/размера делает `FlushRenderingCommands()` — корректно по потоку (game thread), но это синхронный стопор всего рендера. См. также 5.2.

---

## 4. Дизайн API и связность модулей

### 4.1 [High] Public-заголовок тянет CUDA/NVENC
`Public/NvEncEncoder.h:4-5` включает `<cuda.h>` и `nvEncodeAPI.h`. Любой потребитель, включающий этот public-хедер, обязан иметь CUDA/NVENC SDK в include-путях. Нарушает дух marketplace-правила (не экспонировать тяжёлые third-party хедеры). Лечится pimpl/opaque или переносом в `Private/`.

### 4.2 [High] Нет платформенных guard'ов — модуль Linux-only по факту
`Build.cs` не содержит веток Win/Mac/Android, а `.cpp` (особенно `GstH264AppSrcComponent.cpp`, все `Gst*Impl.cpp`) безусловно включают `<gst/gst.h>`, `<cuda.h>`, `<vulkan.h>`, `<unistd.h>`. Без `#if PLATFORM_LINUX` и веток сборки модуль не компилируется нигде, кроме Linux. Это блокирует цель кросс-платформенности и Fab-валидацию `BuildPlugin` на Win. (Связано с переходным состоянием: модуль ещё не вынесен в отдельный плагин — но платформенные ветки понадобятся независимо от выноса.)

### 4.3 [Med] Два несвязанных пути отправки
`UGstH264AppSrcComponent` (zero-copy, замкнут на себе: сам создаёт SceneCapture-target, NVENC, pipeline) vs `UGstAppSrcComponent` (generic CPU readback, композиция через `UGstPipelineComponent`). Разные модели буферов, разные жизненные циклы, нет общей абстракции источника. Усложняет проектирование user-facing API (открытый вопрос в CLAUDE.md).

### 4.4 [Med] Инвертированное именование interface/impl
Интерфейсы лежат в файлах `*Impl.h`: `IGstPipeline` в `Public/GstPipelineImpl.h`, `IGstAppSrc` в `GstAppSrcImpl.h` и т.д. «Impl» обычно означает реализацию; здесь это публичные интерфейсы, а реализации — в `Private/*Impl.cpp`. Сбивает с толку.

### 4.5 [Med] `USharedRenderTarget2D::ExternalMemoryFd` — мёртвое поле
`SharedRenderTarget2D.h:14` объявлено, но нигде не присваивается (только `ExternalSemaphoreFd` пишется в `:537`). `SharedRenderTarget2D.cpp` пустой (1 строка). Похоже на остаток ранней итерации.

### 4.6 [Low] Пустые override-заглушки
`GstElementComponent.cpp:8-31`: `InitializeComponent/BeginPlay/EndPlay/UninitializeComponent/TickComponent` лишь вызывают `Super::` — мёртвый boilerplate. Тик отключён (`:5`), но методы есть.

### 4.7 [Low] `NewUniqueObject` в `SharedUnreal.h` не используется
`Public/SharedUnreal.h:9-15` — шаблон, не вызывается нигде в модуле.

### 4.8 [Low] Хардкод в `Build.cs`
`Build.cs:66` CUDA-путь `/usr/local/cuda-12.9` зашит; `:40-41` `x86_64-linux-gnu` зашит (ломается на arm64/иных дистрибутивах). Линковка системных `.so` по абсолютным путям без `RuntimeDependencies`/delayload (см. 5.5).

---

## 5. Производительность и прочее

### 5.1 [Med] Нет пайплайнинга кадров в zero-copy пути
`GstH264AppSrcComponent`: `bEncoding` (`:554`, `:568`) разрешает только один кадр в полёте. Захват кадра N+1 не начинается, пока encode N не завершён. Один NV12-буфер и один bitstream-буфер NVENC не позволяют перекрывать стадии (capture/convert ║ encode). README это признаёт. Для целевых benchmark-чисел — ключевое ограничение пропускной способности/латентности.

### 5.2 [Med] `FlushRenderingCommands` на каждой смене формата приёма
`GstTexture.cpp:200` (через `Resize` → `Release`). При первом кадре и любой смене caps — полный стопор рендера. Для часто меняющегося разрешения дорого.

### 5.3 [Low] `ReadSurfaceData` каждый кадр в copy-пути
`GstAppSrcComponent.cpp:141`: GPU→CPU readback — ожидаемо для copy-пути, но это синхронизационный барьер GPU. Это и есть та точка, ради которой делается zero-copy.

### 5.4 [Low] SEI-таймстамп собирается побайтовыми циклами каждый кадр
`GstH264AppSrcComponent.cpp:707-712`: ручная сборка 40-байтового SEI на каждый кадр. Стоимость ничтожна; отмечено для полноты. Также `static const uint8 SEI_UUID` дублирует семантику — стоит вынести формат в одно место, если SEI станет частью контракта.

### 5.5 [Med] Marketplace-упаковка не настроена
`Build.cs`: GStreamer/glib/Vulkan/CUDA линкуются как системные `.so` по абсолютным путям; через `RuntimeDependencies` идёт только `libcudart.so.12` (`:71`). Для Fab/распространения нужны `RuntimeDependencies` (`StagedFileType.NonUFS`) для бандлящихся бинарников и delayload-стратегия. На Linux часть либ — системные (ок для open-source GitHub-сборки), но для installed-engine `BuildPlugin` это надо пересмотреть. Связано с 4.2.

### 5.6 [Low] `GST_ENABLE_DEBUG_LOG` включён в `Shared.h`
`Public/Shared.h:6`: `#define GST_ENABLE_DEBUG_LOG` всегда активен → debug-логи компилируются в релиз. `StartupModule` (`GStreamerModule.cpp:24`) ещё и ставит `EGstVerbosity::Debug` по умолчанию — шумно для конечного пользователя.

---

## Сводка по приоритету (High)

- 1.1 `Flush` не возвращает NAL.
- 1.2 Двойной destroy + Vulkan-вызовы вне render thread.
- 1.3 Создание `UTexture2D` на render thread.
- 2.1 Утечка fd при неуспешном CUDA-импорте.
- 3.1 Гонка на пуле сэмплов (`AllocSample`).
- 3.2 Use-after-free при teardown appsink с живым callback.
- 4.1 CUDA/NVENC в public-заголовке.
- 4.2 Нет платформенных веток — модуль Linux-only, блокирует кросс-платформенность и Fab.
