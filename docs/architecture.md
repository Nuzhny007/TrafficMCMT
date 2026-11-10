# TrafficMCMT — архитектура

> **Статус документа:** живая спецификация. Обновляется вместе с кодом.
> **Версия:** 0.4 (Этап 2b: map-feed и viewer-протокол)
> **Дата:** 2026-11-05

## 1. Назначение и границы

TrafficMCMT — **Master-узел** системы многокамерного трекинга (Multi-Camera Multi-Target,
MCMT), реализующий **late fusion**: он не работает с пикселями и не выполняет MOT для
отдельной камеры. Он принимает уже готовые **треклеты** (фрагменты траекторий с
эмбеддингами) от Edge-узлов, выполняет межкамерный Re-identification, пространственно-
временную фильтрацию и глобальную ассоциацию, выдавая **стабильные глобальные ID**.

```
Edge (Multitarget-tracker)  →  треклеты  →  Master (TrafficMCMT)  →  глобальные ID
   детекция, локальный MOT                    ReID + st-ReID + min-cost flow
```

### 1.1. Что делает Edge

| Функция | Компонент |
|---|---|
| Детекция объектов | `mdetection::BaseDetector` (YOLO / D-FINE / TensorRT / BGFG) |
| Настройки детектора | `TrackerSettings` из `configs/detectors/*.ini` (`[detection]`, `[tracking]`) |
| Локальный MOT | `mtracking::BaseTracker` (Kalman + Hungarian) |
| Локальный `track_id` | `TrackingObject::m_ID` |
| Геопривязка кадра | `tmcmt::GeoBinding` (`geo_bind.ini` → ENU, метры) |
| Извлечение ReID-эмбеддинга | `tmcmt::TypeAwareReIdExtractor` / `IReIdExtractor` |
| Формирование треклетов | `tmcmt::EdgeTrackExporter` |
| Публикация в сеть | `tmcmt::ZmqTrackletPublisher` |

### 1.2. Что делает Master

| Функция | Модуль |
|---|---|
| Приём и нормализация треклетов | `ingest/` |
| Хранение истории | `storage/` |
| Векторный поиск эмбеддингов | `reid/` |
| Spatio-temporal ограничения | `st/` |
| Глобальная ассоциация (min-cost flow) | `association/` |
| Глобальные ID и события merge | `association/GlobalIdManager` |
| BEV и мировые координаты | `bev/` |
| Выдача результатов | `output/` |
| Метрики качества | `metrics/` |

### 1.3. Жёсткие архитектурные правила

1. **Master не зависит от Multitarget-tracker и OpenCV.** Связь с MTT существует только в
   таргете `trafficmcmt_mtt` (Edge-экспортёр и утилита `mtt_export`).
2. **Edge не знает о глобальных ID.** Master не видит кадров.
3. **Все внешние решатели — за интерфейсами** (`IEmbeddingIndex`, `IMcfSolver`,
   `ITrackletSource`, `IResultSink`). Замена FAISS/muSSP/ZeroMQ не должна затрагивать
   бизнес-логику.
4. **Закоммиченный глобальный ID не меняется и не переиспользуется.** Изменение
   оформляется событием `GlobalIdMergeEvent`.

## 2. Окружение (проверено)

| Компонент | Расположение | Версия / примечание |
|---|---|---|
| Multitarget-tracker (исходники) | `C:\work\home\mtracker\TrafficMCMT\Multitarget-tracker` | master, локальный клон — можно править |
| Multitarget-tracker (install) | `...\Multitarget-tracker\install` | 1.1.0, MSVC 14.51.36231 (VS 2026), OpenCV 4.12 |
| CMake-конфиг MTT | `...\install\lib\MTTracking\cmake` | `MTTracking_DIR` |
| CMake-таргеты MTT | `MTTracking::mtracking`, `MTTracking::mdetection`, `MTTracking::inih` | `mtracking` → только `inih`; `mdetection` → OpenCV DNN |
| Include MTT | `MTTracking_INCLUDE_DIR` | интерфейсных include-путей у таргетов нет |
| OpenCV | `C:\work\tools\vcpkg\installed\x64-windows\share\opencv4` | 4.12.0, ABI совпадает с `mtracking.lib` |
| Protobuf | `C:\work\tools\vcpkg\installed\x64-windows\share\protobuf` | 33.4.0, `protobuf::libprotobuf` + `protobuf::protoc` |
| spdlog | `C:\work\libraries\spdlog\rel\install` | `spdlog::spdlog` |
| yaml-cpp | `C:\work\libraries\yaml\yaml-cpp_build\install` | `yaml-cpp::yaml-cpp` |
| GTest | `C:\work\libraries\gtest\rel\install` | `GTest::gtest_main` |
| Eigen | `C:\work\libraries\eigen-3.4.0` | header-only |
| ZeroMQ / FAISS / muSSP | — | **не установлены**, подключаются на Этапах 3, 5, 6 |

Следствия:

- Master собирается **CPU-only** и не линкует `MTTracking::mdetection`.
- `TrackingObject` не содержит эмбеддинга и timestamp → Edge-обвязка добавляет их сама.
- `mtracking/EmbeddingsCalculator.hpp` в установленных заголовках закрыт
  `#ifdef USE_OCV_EMBEDDINGS`, который потребителю не пробрасывается → Edge использует
  собственный `IReIdExtractor`.
- MTT пересобран из локального клона master тем же компилятором, что и TrafficMCMT
  (`MSVC 14.51.36231`, VS 2026), с `USE_OCV_KCF=OFF`, `USE_OCV_UKF=OFF`,
  `USE_OCV_EMBEDDINGS=OFF`, потому что в vcpkg OpenCV 4.12 нет `opencv_tracking`,
  а `USE_OCV_EMBEDDINGS` добавлял зависимость от `opencv_dnn` с другим inline-namespace.
- В актуальном master MTT класс `CTracker` заменён на `BaseTracker`
  (`mtracking/BaseTracker.h`), `CreateTracker(settings, fps)` принимает fps, а
  `UpdateMat` — `time_point_t` вместо fps. Экспортёр использует новый API.
- `mdetection` собирается без CUDA/TensorRT: достаточно OpenCV DNN, он нужен только
  для полноты экспортируемого набора MTT.
- `trafficmcmt_mtt_export` — единственное приложение, которое линкует
  `MTTracking::mdetection`; `trafficmcmt_mtt` и Master остаются без него.
- Детектор создаётся через `BaseDetector::CreateDetector` с `config_t` из
  `configs/detectors/*.ini`; поддерживаются `YOLOV8`, `YOLO11`, `DFINE` на COCO.
  Лишние классы отключаются `white_list` (в MTT это `;`-список типов).
- Геопривязка реализована в `trafficmcmt_edge` (`GeoBinding`) и не зависит от
  примеров MTT; формулы WGS84→ENU совпадают с `LocalPlane` из MTT.

### 2.1. Проверенная сборка

| Параметр | Значение |
|---|---|
| Компилятор | MSVC 19.51.36231 (Visual Studio 2026) |
| CMake | 4.3.2, генератор `Visual Studio 18 2026` |
| Конфигурация | `cmake --preset local-release` |
| Результат | `trafficmcmt_core`, `trafficmcmt_proto`, `trafficmcmt_io`, `trafficmcmt_edge`, `trafficmcmt_mtt`, `trafficmcmt_master`, `trafficmcmt_mtt_export`, `trafficmcmt_tracklet_inspect` |
| Тесты | `ctest --preset local-release` → 84/84 passed |
| Зависимости Master | только runtime MSVC/Windows — без OpenCV, CUDA, MTT |

Опции CMake по умолчанию: `MTT=ON`, `ZMQ=OFF`, `PROTOBUF=OFF`, `FAISS=OFF`,
`MUSSP=OFF`, `TESTS=ON`. Тяжёлые зависимости включаются на своих этапах; при
отсутствии пакета опция автоматически выключается с предупреждением, сборка не падает.

## 3. Топология

```
┌──────────────────── EDGE NODE (Jetson / PC, 1 камера) ────────────────────┐
│  VideoCapture                                                             │
│      ▼                                                                    │
│  mdetection::BaseDetector ──► mtracking::BaseTracker ──► GetTracks()      │
│                                                              │            │
│                        ┌─────────────────────────────────────┘            │
│                        ▼                                                  │
│  EdgeTrackExporter: bbox + trace + conf + type + ts                       │
│                        ▼                                                  │
│  ReIdExtractor: crop → 512-d L2-normed, EMA по треку                      │
│                        ▼                                                  │
│  TrackletChunker: 1 Гц / по завершении трека → TrackletChunk              │
│                        ▼                                                  │
│  ZmqTrackletPublisher ── tmcmt.v1.<camera_id>.tracklet ──┐                │
└───────────────────────────────────────────────────────────┼───────────────┘
                                                            │ tcp://edge:5555
                                            ┌───────────────▼──────────────┐
                                            │ BROKER (опционально)         │
                                            │ zmq_proxy(XSUB, XPUB)        │
                                            └───────────────┬──────────────┘
                                                            │ SUB
┌─────────────────────────── MASTER NODE: TrafficMCMT ──────▼──────────────────┐
│  ITrackletSource ──► TimestampBuffer ──► TrackletNormalizer ──► TrackletStore│
│  (Zmq | File)          (watermark)         (валидация, dedup)    (RAM+SQLite)│
│                                                                      │       │
│  ClockSyncEstimator ──────────────────────────────► GalleryManager    │       │
│                                                             ▼        │       │
│                                                    IEmbeddingIndex (FAISS)   │
│                                                             ▼                │
│  CameraLinkModel ──► SpatialGate ──► StReIdScorer ──► PairCandidateGenerator │
│                                                             ▼                │
│                                              TrackletGraph + GraphPruner     │
│                                                             ▼                │
│                                              IMcfSolver (muSSP / SSP)        │
│                                                             ▼                │
│                                              FlowAssociator → GlobalIdManager│
│                                                             ▼                │
│  IResultSink: JSONL | CSV | SQLite | ZMQ PUB | Prometheus                    │
└──────────────────────────────────────────────────────────────────────────────┘
```

## 4. Потоки данных

| Поток | Содержимое | Формат | Частота | Объём (оценка) |
|---|---|---|---|---|
| Detections → Tracker | `regions_t` | in-process | 25 Гц | — |
| Edge → Master | `TrackletChunk` | Protobuf / ZMQ multipart | 1 Гц на трек + `TRACK_END` | 100 треков × 2.9 КБ ≈ 290 КБ/с на камеру |
| Edge → Master | `CameraStatus` | Protobuf | 1 Гц | ~200 Б/с |
| Edge → Master | `CameraCalibration` | Protobuf / YAML | при старте/изменении | ~1 КБ |
| Master → потребители | `GlobalTrackUpdate` | Protobuf / JSONL | 1 Гц на глобальный трек | ~100 КБ/с |
| Master → Prometheus | счётчики/гистограммы | HTTP /metrics | scrape 15 с | — |

При 10 камерах × 100 треков × 1 Гц: **~2.9 МБ/с** (float32) или **~1.3 МБ/с** (int8).

## 5. Режимы работы

| Режим | Источник | Ассоциация | Качество | Применение |
|---|---|---|---|---|
| `offline` | `FileTrackletSource` | полный граф сессии | эталонное | разбор инцидентов, валидация алгоритмов |
| `online` | `ZmqTrackletSource` | скользящее окно `W` + `commit_lag` | близко к offline | реальное время |

Онлайн-режим: окно `window_ms` (30 с), перекрытие `window_overlap_ms` (10 с),
заморозка ID старше `commit_lag_ms` (15 с). Пере-решение окна не меняет закоммиченные ID,
только добавляет новые или порождает `GlobalIdMergeEvent`.

## 6. Модули и публичный API

| Таргет | Ключевые классы | Зависимости |
|---|---|---|
| `trafficmcmt_core` | `Tracklet`, `CameraCalibration`, `MasterConfig`, `Result<T>`, `Log` | spdlog, yaml-cpp |
| `trafficmcmt_proto` | сгенерированные `tracklet.pb.*`, `calibration.pb.*` | protobuf |
| `trafficmcmt_io` | `TrackletCodec`, `FileTrackletSource`, `TrackletNormalizer`, `InMemoryTrackletStore`, `JsonlTrackletSink`, `BinaryTrackletSink` | protobuf |
| `trafficmcmt_edge` | `IReIdExtractor`, `ColorHistogramReIdExtractor`, `DnnReIdExtractor`, `TypeAwareReIdExtractor`, `GeoBinding`, `EdgeTrackExporter` | OpenCV (core, imgproc, dnn, calib3d) |
| `trafficmcmt_reid` | `IEmbeddingIndex`, `BruteForceIndex`, `FaissIndex`, `EmbeddingNormalizer`, `GalleryManager`, `ReIdMatcher` | FAISS (opt.), Eigen |
| `trafficmcmt_st` | `CameraLinkModel`, `TransitionEstimator`, `SpatialGate`, `StReIdScorer`, `PairCandidateGenerator`, `TrackletEndpoint`, `Polygon` | core, reid (Eigen) |
| `trafficmcmt_flow` | `TrackletGraph`, `GraphPruner`, `MuSspSolver`, `SspSolver`, `GreedyAssociator`, `FlowAssociator`, `GlobalIdManager` | core, muSSP (opt.) |
| `trafficmcmt_bev` | `HomographyProjector`, `WorldTrajectoryFusion` | OpenCV core, Eigen |
| `trafficmcmt_net` | `ZmqTrackletSource`, `ZmqTrackletPublisher`, `ZmqResultSink` | libzmq/cppzmq, protobuf |
| `trafficmcmt_map` | `MapConfig`, `MapFeed`, `MapProtocol`, `TrackletTimeline`, `VelocityEstimator`, `TrailBuilder`, `MapIdRegistry` | core, yaml-cpp |
| `trafficmcmt_emissions` | `EmissionSimulator`, `HbefaEmissionModel`, `PasquillGifford`, `GaussianPuffModel`, `ConcentrationGrid` | core |
| `trafficmcmt_pipeline` | `MasterPipeline`, `WindowScheduler` | все выше |
| `trafficmcmt_mtt` | `MttTrackAdapter` | **MTTracking::mtracking**, OpenCV |
| `trafficmcmt_metrics` | `MotMetrics` — track-level IDF1/IDP/IDR/IDSW, Hungarian | core |

Приложения Этапа 2: `trafficmcmt_mtt_export` (видео → треклеты; единственный таргет,
линкующий `MTTracking::mdetection`) и `trafficmcmt_tracklet_inspect` (статистика и
JSON-дамп треклетов). Этап 2b/2c добавляет `trafficmcmt_map_export` (треклеты →
JSONL-кадры v1) и `trafficmcmt_map_gateway` (WebSocket-реплей кадров и выбросов).
Этап 3 добавляет `trafficmcmt_reid_eval` (CMC/Rank-1/mAP) и `trafficmcmt_reid_bench`
(латентность и recall индекса). Этап 4 добавляет `trafficmcmt_st_estimate`
(размеченные пары → статистика переходов → `camera_links.yaml`). Этап 5 добавляет
`trafficmcmt_associate` (offline: треклеты → глобальные ID, опционально IDF1/IDSW).

### 6.1. Публичный контракт

Публичным считается только то, что перечислено ниже; остальное — внутренняя реализация.

```cpp
namespace tmcmt {

struct MasterConfig;                       // core/Config.h

class MasterPipeline {                     // pipeline/MasterPipeline.h
public:
    static Result<std::unique_ptr<MasterPipeline>> Create(const MasterConfig&, std::shared_ptr<spdlog::logger>);
    Status Start();
    Status RunOffline();
    Status RunOnline();
    void   Stop();
    PipelineStats Stats() const;
};

class ITrackletSource;                     // ingest/ITrackletSource.h
class IResultSink;                         // output/IResultSink.h
class ITrackletStore;                      // storage/ITrackletStore.h
class IEmbeddingIndex;                     // reid/IEmbeddingIndex.h
class IMcfSolver;                          // association/IMcfSolver.h

} // namespace tmcmt
```

## 7. Протокол обмена (v1)

Полная схема — `proto/tracklet.proto`, `proto/calibration.proto` (Этап 2).
Здесь фиксируются инварианты.

### 7.1. Обязательные поля `TrackletChunk`

| Поле | Тип | Инвариант |
|---|---|---|
| `session_id` | string | UUID запуска Edge, не пустой |
| `camera_id` | string | стабильный ID камеры, не пустой |
| `local_track_id` | uint64 | уникален в рамках `(session_id, camera_id)` |
| `sequence` | uint64 | монотонно растёт на камеру |
| `begin_ms`, `end_ms` | int64 | UTC мс, `begin <= end` |
| `observations` | repeated | ≥ 1, отсортированы по `timestamp_ms` |
| `embedding` | message | `dim > 0`, L2-норма ≈ 1 |
| `schema_version` | uint32 | сейчас `1` |

### 7.2. Топики ZeroMQ

Формат: `tmcmt.<major>.<camera_id>.<kind>`, сообщение — multipart `[topic][payload]`.

| Топик | Payload |
|---|---|
| `tmcmt.v1.<cam>.tracklet` | `TrackletChunk` |
| `tmcmt.v1.<camera>.status` | `CameraStatus` |
| `tmcmt.v1.calib` | `CameraCalibration` |
| `tmcmt.v1.global.track` | `GlobalTrackUpdate` |
| `tmcmt.v1.global.events` | `GlobalIdMergeEvent` |

Параметры сокетов: `SNDHWM=1000`, `RCVHWM=5000`, `LINGER=0`,
`RECONNECT_IVL=1000`, `HEARTBEAT_IVL/TIMEOUT=1000/3000`.

### 7.3. Гарантии доставки

PUB/SUB — best effort. Компенсация: `sequence` + `SequenceTracker` (метрика
`tmcmt_tracklets_missing_total`), опциональный спулинг на Edge с досылкой.

### 7.4. Файловый формат (Этап 2)

Один и тот же `TrackletChunk` сериализуется в три формы:

| Формат | Расширение | Кодирование |
|---|---|---|
| JSONL | `.jsonl` | одна строка — protobuf JSON (`google::protobuf::util`), поля в `snake_case` |
| Framed protobuf | `.bin`, `.pb` | `varint(length) + payload`; поток читается до EOF |
| Raw protobuf | — | один `TrackletChunk` на сообщение (только API) |

- `schema_version` проверяется на входе: несовместимая версия → `ErrorCode::Unsupported`.
- `Embedding` хранится как `float` или `int8` + `scale`; квантование включается
  `reid.quantize_embeddings` / `--quantize`, при чтении значения восстанавливаются.
- `FileTrackletSource` определяет формат по расширению или первому байту
  (`auto`), поддерживает `Reset()` и реплей с ускорением (`speed=0` — без паузы).
- `TrackletNormalizer` проверяет инварианты §7.1, сортирует наблюдения, расширяет
  `[begin, end]` до фактического диапазона, L2-нормирует эмбеддинг, ограничивает
  `quality` и отбрасывает дубликаты по `(session, camera, local_track_id, sequence)`.

### 7.5. Мировые координаты и временная база Edge

**Геопривязка.** `GeoBinding` читает секцию `[points]` из формата
Multitarget-tracker `geo_bind.ini`:

```ini
[points]
lat0 = 45.52724575
lon0 = 5.97433625
px_x0 = 0.1234      ; нормализованный пиксель x / width
px_y0 = 0.5678      ; нормализованный пиксель y / height
```

- Пиксели нормализованы в `[0, 1]`, поэтому привязка не зависит от разрешения.
- WGS84 → локальная плоскость East-North-Up (метры) по формулам эллипсоида
  (эквивалент `LocalPlane` в MTT). По умолчанию начало координат — среднее
  широт/долгот контрольных точек.
- Для нескольких камер начало координат **должно совпадать**, иначе их мировые
  координаты сдвинуты друг относительно друга. Общий origin задаётся
  `--geo-origin lat,lon` (или ключами `origin_lat`/`origin_lon` в `[points]`).
- Гомография «нормализованный пиксель → ENU» оценивается `findHomography`
  (≥ 4 точек). Обратная матрица используется для `ToNormalizedPixel`.
- RMS-ошибка проверяется в метрах (`max_rms_error_m`, по умолчанию 10 м) и в
  нормализованных пикселях (`max_rms_error_px`, по умолчанию 0.02).
- В `mtt_export` нижняя центральная точка bbox проецируется в метры и попадает
  в `Observation.world`; та же точка передаётся в `CRegion::SetLocalCartesian`,
  чтобы локальный трекер мог матчить по `DistLocalCartesian` (порог `--local-dist`, м).

**Временная база.** `TrackingObject` не хранит timestamp, поэтому экспортёр
вычисляет его сам:

```
timestamp_ms = start_epoch_ms + round((start_frame + frame_index) * 1000 / fps)
```

- `--start-epoch-ms` задаёт абсолютное время первого кадра; для нескольких камер
  он должен быть одинаковым, иначе межкамерные `Δt` бессмысленны.
- `--start-frame` позволяет начать с середины видео, сохраняя корректное время.
- `session_id` по умолчанию `edge-<start_epoch_ms>`, `camera_id` — `--camera`.
- Все наблюдения и чанки используют `std::chrono::milliseconds` UTC.

**ReID по типу объекта.** `TypeAwareReIdExtractor` выбирает модель по
`ObjectType` и возвращает разные `model_id`:

| Тип | Модель | `model_id` по умолчанию |
|---|---|---|
| `Pedestrian` | OSNet/ONNX, иначе HSV | 2 |
| `Car` / `Truck` / `Bus` / `Bike` | ONNX, иначе HSV | 3 |
| `Unknown` | HSV | 1 |

Разные `model_id` не дают Master смешивать несравнимые дескрипторы. DNN-ветка
использует ImageNet mean/std (`0.485/0.456/0.406`, `0.229/0.224/0.225`) и
`swapRB`, вход по умолчанию `128x256`.

### 7.6. Map-feed: треки, траектории и скорость для viewer

Поток данных (Этап 2b, без MTT/OpenCV/CUDA в Master):

```
mtt_export ──> TrackletChunk (JSONL/protobuf)
                    │
                    ▼
        TrackletTimeline ──> MapFeed ──> MapFrame (protocol v1)
                    │                       │
             map_export                map_gateway ──> WebSocket ──> viewer
             (JSONL)                   (replay/real-time)
```

**Общий ENU origin.** Поля `origin_lat = 13`, `origin_lon = 14` добавлены в
`TrackletChunk` (аддитивно, старые файлы читаются как `0`). `Tracklet::HasOrigin()`
и `ResolveMapOrigin()` действуют так:

| Конфиг | Поток | Результат |
|---|---|---|
| задан | нет | origin из конфига, `from_stream=false` |
| нет | задан | origin из потока, `from_stream=true` |
| задан | задан | значения должны совпасть (1e-9), иначе ошибка |
| нет | нет | ошибка с подсказкой задать `feed.origin` |
| — | разные | ошибка «stream mixes ENU origins» |

Единственный источник формул WGS84↔ENU — `core/Geo.h`; `GeoBinding` и map-слой
делегируют туда, поэтому Edge и Master считают метры одинаково.

**Модули.** `trafficmcmt_map` (без сокетов и OpenCV):

| Компонент | Назначение |
|---|---|
| `MapConfig` | YAML `feed`/`cameras`, зоны из `[points]` (`geo_bind`) как выпуклая оболочка |
| `TrackletTimeline` | слияние чанков в один поток наблюдений по timestamp |
| `MapIdRegistry` | последовательные `global_id` (≤ 2⁵³), стабильные для потока |
| `VelocityEstimator` | МНК по окну 1 с, fallback на конечную разность, кламп 70 м/с |
| `TrailBuilder` | ≤ 40 точек, прореживание по 0.3 м |
| `MapFeed` | наблюдения → `tracks_update` / `tracks_remove` / `camera_zones` |
| `MapProtocol` | компактный JSON v1 (`position=[lon,lat]`, `velocity=[vx,vy]` м/с) |

`trafficmcmt_net` — минимальный RFC 6455 сервер/клиент на платформенных сокетах
(Winsock/POSIX), без сторонних зависимостей: handshake, text/ping/pong/close,
`/health`, очередь на клиента (медленный клиент не блокирует продюсера).

**Протокол.** Используется существующий v1 viewer-протокол с аддитивными
расширениями:

- `tracks_update.tracks[].speed_kmh` — модуль скорости в км/ч; если поля нет,
  viewer считает `hypot(velocity) * 3.6`;
- `tracks_remove {ids:[...]}` — немедленное удаление истёкших треков; старые
  клиенты игнорируют неизвестный тип, TTL остаётся страховкой;
- `id_merge` сохраняет прежнюю семантику (перенос trail на новый id).

`tracks_update.mode=snapshot` отправляется новому клиенту вместе с
`camera_zones`; дальше идут только дельты. `MapFeed::Tick` сначала удаляет
истёкшие треки и лишь затем формирует обновления, поэтому мёртвый трек не
получает финальный кадр.

**Конфигурация.** `configs/map_feed.yaml` (Auburn, две камеры) и
`configs/map_feed_aire.yaml` (Aire). Пример:

```yaml
feed:
  origin: 32.606456,-85.481891
  tick_hz: 10
  trail_points: 40
  trail_min_distance_m: 0.3
  velocity_window_s: 1.0
  velocity_max_mps: 70.0
  track_ttl_s: 3.0
  heartbeat_s: 5.0

cameras:
  - id: cam-01
    camera_id: 1
    color: [56, 189, 248, 55]
    geo_bind: ../data/auburn_toomer/sync_videos/1_2021-05-09_22_24.ini
```

`camera_id` — числовой ID для viewer; `id` — строковый ID Edge. Относительные
пути `geo_bind` ищутся от текущего каталога и от каталога конфига.

**Запуск.**

```powershell
# offline: кадры в JSONL
trafficmcmt_map_export --input build/datasets/auburn_toomer/combined.jsonl \
  --config configs/map_feed.yaml --out build/map/auburn_frames.jsonl

# WebSocket-реплей (1x)
trafficmcmt_map_gateway --input build/datasets/auburn_toomer/combined.jsonl \
  --config configs/map_feed.yaml --port 8080 --speed 1

# smoke с реальным клиентом
powershell -ExecutionPolicy Bypass -File scripts/smoke_map_gateway.ps1
```

`map_gateway` поддерживает `--loop`, `--exit-on-end`, `--duration`, `--speed 0`
(без pacing) и `--frames-out` для отладки. `/health` возвращает
`{"status":"ok","protocol":1,"active_tracks":N}`.

### 7.7. Gaussian puff: выбросы и концентрации (Этап 2c)

Модуль `trafficmcmt_emissions` превращает треки `MapFeed` в карту приземных
концентраций пяти загрязнителей. Он не знает про транспорт: его двигает
`MapFeed::Tick`, а тесты — синтетическими источниками. MTT/OpenCV/CUDA не
участвуют, правило ADR-001 сохраняется.

```
MapFeed::Tick (10 Гц)
   │  TrackState{position, velocity, acceleration}
   ▼
EmissionSimulator (trafficmcmt_emissions)
   │  Step(): спавн puff'ов (puff_interval_s), адвекция, старение
   │  BuildSnapshot(): растеризация ConcentrationGrid (grid_hz)
   ▼
MapFrame{EmissionGrid} ──> MapProtocol ──> WebSocket ──> viewer
```

**Компоненты.**

| Компонент | Назначение |
|---|---|
| `EmissionTypes` | `Pollutant` (NOx, CO, PM2.5, PM10, CO2), `EmissionRates` (г/с), `EmissionSource`, `EmissionFieldStats` |
| `IEmissionModel` | интерфейс модели: класс + скорость + ускорение → ставки г/с |
| `HbefaEmissionModel` | порт HBEFA из SUMO, 3 класса; см. `docs/emissions-attribution.md` |
| `PasquillGifford` | `σ_y = a·x^b`, `σ_z = c·x^d` для классов A–F |
| `GaussianPuffModel` | `PuffState`, адвекция, `ComputePuffGeometry`, `ConcentrationAt` |
| `ConcentrationGrid` | регулярная ENU-сетка, `AddPuff`, `Stats` |
| `EmissionConfig` | секция `emissions:` в `map_feed.yaml` |
| `EmissionSimulator` | население puff'ов, спавн, снапшоты |

**Физика.**

- Ветер задан метеорологическим направлением `from_deg` (0° — с севера,
  90° — с востока); вектор адвекции направлен противоположно.
- Puff движется аналитически: `center = origin + wind_vector · age`, без
  накопления ошибки.
- Аргумент коэффициентов — пройденный по ветру путь
  `x = max(wind_speed, 0.3 м/с) · age` в **метрах**; `σ_x = σ_y`.
- Концентрация в точке (`g/m³`):

  ```
  C = M / ((2π)^1.5 · σx·σy·σz)
      · exp(-Δx²/(2σx²) - Δy²/(2σy²))
      · [exp(-(z-H)²/(2σz²)) + exp(-(z+H)²/(2σz²))]
  ```

  где `M` — масса puff'а, `H` — высота источника, второе слагаемое —
  отражение от земли. Наблюдение ведётся на высоте дыхания `z = 1.5 м`.
- Масса puff'а: `M = rate_gps · puff_interval_s`; пуфы старше `max_age_s`
  удаляются, при переполнении — самые старые.
- `PM2.5 = PMx · pm25_fraction` (по умолчанию 0.5), `PM10 = PMx`.

**Модель выбросов.** Класс трека отображается в строку HBEFA:

| `class` | HBEFA |
|---|---|
| `car`, `unknown` | `SVE_P_LDV_7_7` |
| `truck`, `bus` | `SVE_HDV_6_6` |
| `motorbike` | `SVE_P_LDV_7_1` |
| `bike`, `person` | нулевые ставки |

Ускорение берётся из `AccelerationEstimator` (МНК по окну
`acceleration_window_s`, EMA, кламп `±acceleration_max_mps2`). Отрицательное
ускорение по умолчанию клампится к нулю (`clamp_negative_acceleration`), иначе
карта «рвётся» при торможении. Скорость в `Rates` ограничена 130 км/ч.

**Сетка.** Прямоугольник `width_m × height_m` с шагом `cell_m` (по умолчанию
600×450 м, 5 м → 120×90), центр — `emissions.grid.center_*` или ENU-origin.
Чтобы puff не «проваливался» между узлами, в `ConcentrationGrid::AddPuff`
сигмы клампятся снизу к половине ячейки (`σ_x ≥ 0.5·cell_x`,
`σ_y ≥ 0.5·cell_y`, `σ_z ≥ 0.5·max(cell_x, cell_y)`), после чего
`inverse_norm` пересчитывается. `Stats()` даёт `max`, `mean` по всей сетке и
`p95` только по ненулевым ячейкам (иначе фон из нулей занижает перцентиль).

**Протокол.** Аддитивное расширение v1 — кадр `emission_grid`:

```json
{
  "type": "emission_grid",
  "timestamp": 1793776800000,
  "unit": "ug/m3",
  "bounds": {"west": 5.97, "south": 45.52, "east": 5.98, "north": 45.53},
  "cols": 120,
  "rows": 90,
  "fields": {
    "NOx": {"data": "<base64 f32 LE>", "max": 719.0, "mean": 1.2, "p95": 169.0}
  }
}
```

`data` — row-major `float32` little-endian, строки с юга на север, длина
ровно `cols × rows`. Все пять полей присутствуют всегда. Кадры идут с
частотой `grid_hz` (1 Гц); `map_gateway` дополнительно отправляет последний
снапшот новому клиенту после `camera_zones` и `tracks_update`-снапшота.
Viewer рисует поле через `BitmapLayer` с логарифмической палитрой на каждый
загрязнитель.

**Конфигурация.**

```yaml
emissions:
  enabled: true
  stability_class: D
  wind:
    speed_mps: 3.0
    from_deg: 270
  source_height_m: 0.5
  breathing_height_m: 1.5
  puff_interval_s: 1.0
  max_age_s: 180
  max_puffs: 3000
  pm25_fraction: 0.5
  grid_hz: 1.0
  clamp_negative_acceleration: true
  grid:
    center_lat: 45.52724575
    center_lon: 5.97433625
    width_m: 600
    height_m: 450
    cell_m: 5
```

`enabled: false` или отсутствие секции полностью отключают симулятор —
`MapFeed` не тратит время на puff'ы и не отправляет `emission_grid`.

## 8. Алгоритмические решения

### 8.1. Стоимость связи треклетов

```
c_app   = clamp(1 - cos(e_a, e_b), 0, 2)                       ∈ [0, 2]
c_time  = clamp(z²/2, 0, 4.5) / 4.5,  z = (Δt − μ_ab)/σ_ab     ∈ [0, 1]
c_space = min(1, d_bev / (v_max · max(Δt, ε)))                 ∈ [0, 1]
c_total = (w_a·c_app + w_t·c_time + w_s·c_space) / (w_a + w_t + w_s)
```

`c_time` — это `−ln N(Δt; μ, σ²)` с вычтенной константой `ln(σ√2π)` и нормировкой
на значение в точке `z = 3`: стоимость равна 0 в среднем и 1 на границе 3σ.
Исходная запись `clamp(−ln pdf, 0, τ_t)/τ_t` в среднем не обнулялась.

Гейты (ребро не строится): `Δt ∉ [μ−3σ, μ+3σ]`, `Δt` вне fallback-окна, камеры
не связаны в `CameraLinkModel`, зоны выхода/входа не согласованы (zone-режим),
`d_bev > v_max·Δt + ε` (BEV-режим), нет валидных world-координат (zone/BEV).
Подробности — §8.5.

### 8.2. Глобальная ассоциация

`TrackletGraph` хранит узлы (`TrackletNode`) и направленные переходы
`out(from) → in(to)` с «сырой» стоимостью `c_total`. Граф ациклический: переход
разрешён только если `from.end ≤ to.begin`; при равных метках — по возрастанию id.

Солверы строят split-сеть:

```
source → in_i     cap 1, cost 0
in_i   → out_i    cap 1, cost 0
out_i  → sink     cap 1, cost 0
out_i  → in_j     cap 1, cost c_total(i,j) − λ
```

где `λ = flow.link_reward` (по умолчанию 1.0). Min-cost flow даёт набор
непересекающихся путей; переход выбирается только при отрицательной
эффективной стоимости, то есть при `c_total < λ`. В отчёте `FlowPath::cost` и
`total_cost` остаются суммой сырых `c_total`, поэтому метрика не зависит от
награды за связь. `TrackletGraph::SetLinkReward()`/`LinkReward()` переносят
параметр в прунер и компоненты.

- `GraphPruner` — dedupe `(from, to)` с минимальной стоимостью, top-K исходящих
  рёбер (`max_candidates_per_node`), разбиение на слабо связные компоненты и
  пропуск компонент больше `max_component_nodes`.
- `SspSolver` — собственный successive shortest path (эталон корректности и
  fallback); на DAG начальные потенциалы считаются топологической сортировкой.
- `MuSspSolver` — адаптер к muSSP; собирается только при
  `TRAFFICMCMT_WITH_MUSSP=ON`. Локальный форк подключается через
  `TRAFFICMCMT_MUSSP_SOURCE_DIR` (путь к checkout с `muSSP/Graph.cpp`),
  исходники компилируются в object-library и не попадают в install-export.
- `GreedyAssociator` — baseline: рёбра принимаются по возрастанию стоимости,
  пока оба конца свободны; оптимальность не гарантируется.
- `FlowAssociator` — собирает граф, применяет прунер, решает каждую компоненту,
  дополняет непокрытые узлы singleton-путями и передаёт пути в `GlobalIdManager`.
  Если компонента была пропущена по размеру, `optimal=false`.

`flow.solver=greedy|ssp` работает без muSSP. Offline CLI `trafficmcmt_associate`
принимает JSONL/protobuf-треклеты и `camera_links.yaml`, строит переходы через
`PairCandidateGenerator` и пишет JSONL `{tracklet, global_id, camera, begin_ms,
end_ms}`; с `--gt` дополнительно считает IDF1/IDP/IDR/IDSW.

### 8.3. Стабильность ID

`GlobalIdManager`: union-find `TrackletId → GlobalId`; ID начинаются с 1 и
монотонно растут, никогда не переиспользуются. `Link(a, b)` объединяет группы:
если одна сторона уже закоммичена, сохраняется её ID, иначе — меньший.
Слияние оформляется `GlobalIdMergeEvent{old_id, new_id, reason, ts}`.
`Commit(now)` замораживает группы, последний треклет которых старше
`now − commit_lag`; закоммиченный ID не меняется при пере-решении окна.
В offline-режиме `commit_lag` не влияет на назначение ID.

### 8.4. ReID-индекс, галереи и метрики

Модуль `trafficmcmt_reid` — CPU-only: `trafficmcmt_core` + Eigen 3. Он не линкует
OpenCV, protobuf или Multitarget-tracker, поэтому собирается в CI-конфигурации
`EDGE=OFF, MTT=OFF, PROTOBUF=OFF`.

**Индекс эмбеддингов.** `IEmbeddingIndex`:

```cpp
Status Add(TrackletId, std::span<const float>);
Status Remove(TrackletId);                 // NotFound для неизвестного id
Status Rebuild(std::span<const IndexEntry>); // атомарная замена + переобучение IVF
Result<std::vector<SearchHit>> Search(query, k[, filter]);
```

- `SearchHit{id, score}`, `score` — inner product; для L2-нормированных векторов это
  косинус. Сортировка по убыванию score, при равенстве — по возрастанию `id`
  (детерминизм).
- `BruteForceIndex` хранит row-major матрицу `N×dim`, считает `N×dim` на `dim`
  через Eigen и выбирает top-K через `partial_sort` (O(N log k)). При `N ≥ 20 000`
  скан делится на блоки строк и выполняется в нескольких потоках
  (`EmbeddingIndexOptions::threads`, 0 — эвристика, 1 — выключено).
- `Add` заменяет вектор с тем же id; `Remove` — swap-with-last, поэтому остальные
  результаты не меняются. `Rebuild` сначала валидирует весь батч и только потом
  подменяет содержимое: битая запись не портит рабочий индекс.
- `CreateEmbeddingIndex(kind, options)` — фабрика. При
  `TRAFFICMCMT_WITH_FAISS=OFF` запрос `faiss_flat/faiss_ivf_pq/faiss_hnsw`
  возвращает `Unsupported` с явным сообщением, а не тихий fallback на brute force.
- `FaissIndex` (`IndexFlatIP`, `IndexIVFPQ`, `IndexHNSWFlat` + `IndexIDMap2`)
  компилируется только при `TRAFFICMCMT_WITH_FAISS=ON`; на машине разработки
  пакет FAISS не установлен, поэтому бэкенд не проверялся. IVF обучается в
  `Rebuild`; до обучения `Add` складывает векторы в буфер, а `Search` сканирует
  его линейно. `nlist` уменьшается до числа обучающих векторов, `nprobe`
  ограничивается `nlist`.

**Нормализация.** `EmbeddingNormalizer` применяет `y = W·(x − mean)` и L2.
Артефакт (little-endian, IEEE-754):

```
char[8]  "TMCMTWN1"
uint32   in_dim
uint32   out_dim
float    mean[in_dim]
float    matrix[out_dim * in_dim]   // row-major
```

`out_dim == in_dim` — whitening, `out_dim < in_dim` — PCA. `Save`/`Load`
проверяют магию, размеры, конечность значений и отсутствие хвоста файла.

**Галерея.** `GalleryManager` — авторитетное хранилище метаданных и векторов;
индекс — производная структура. Это позволяет точно удалять и вытеснять записи
на любом бэкенде. `GalleryEntry` содержит `camera`, `type`, `model_id`,
`begin/end`, `quality`, `values`.

- `Add(const Tracklet&)` / `Add(const GalleryEntry&)`, `Remove`, `Clear`.
- `Evict(now)` удаляет записи с `end < now − retention_ms`; `retention_ms == 0`
  отключает вытеснение. Индекс `end → id` упорядочен, поэтому свип не сканирует
  всю галерею.
- Камеры хранятся в отдельных bucket-множествах: `exclude_same_camera` и
  `camera`-фильтр не требуют сравнения строк на каждого кандидата.
- `GalleryQuery`: `top_k`, `query_camera`, `exclude_same_camera`, `exclude_id`,
  `camera`, `from/to` (пересечение интервалов), `type`, `model_id`,
  `min_similarity`. Фильтр применяется до top-K, поэтому `top_k` — это уже
  отфильтрованный результат.

**Матчер.** `ReIdMatcher` превращает hits в пары для spatio-temporal этапа:

```
c_app = clamp(1 − similarity, 0, 2)
```

Отсекаются self (`exclude_id`), кандидаты с другим `model_id` (дескрипторы
разных моделей несравнимы), кандидаты ниже `reid.min_similarity` и, по
умолчанию, треклеты своей камеры (`reid.exclude_same_camera`).

**Оценка.** `apps/reid_eval` поддерживает два режима:

- `--tracklets` — chunk-split наших треклетов. Идентичность —
  `(session_id, camera, local_track_id)`; чанки с чётным `sequence` попадают в
  галерею, с нечётным — в запросы. Это проверяет согласованность дескриптора
  внутри трека, а не кросс-камерный ReID.
- `--manifest` — внешний размеченный JSONL:
  `{id, identity, camera, model_id, split: "gallery"|"query", embedding:[...]}`.

Метрики: CMC (Rank-1/5/10) и mAP@R, где `R` — число галерейных чанков той же
идентичности; `AP = (Σ precision@i · rel(i)) / R`. Запросы без галерейной
идентичности пропускаются и считаются в `skipped`.

`apps/reid_bench` генерирует детерминированные единичные векторы, строит индекс,
измеряет `steady_clock` на каждом запросе и печатает p50/p95/p99, QPS и
recall@k относительно точного скана. Google Benchmark не используется.

Локальный замер (Windows, Release, 8 потоков):

| Индекс | Векторы × dim | Запросы | p50 | p95 | p99 | QPS | recall@10 |
|---|---|---|---|---|---|---|---|
| `brute` | 200 000 × 512 | 200 | 12.6 мс | 14.9 мс | 16.2 мс | 77.8 | 1.0 |

Brute force упирается в память: один запрос читает `N·dim·4` байт (410 МБ при
200k×512). Цель p99 < 20 мс на 1M×512 достигается только приближённым индексом
(`faiss_ivf_pq`), а не точным сканом.

### 8.5. st-ReID: топология камер, переходы и пространственные гейты

Модуль `trafficmcmt_st` — CPU-only: `trafficmcmt_core` + `trafficmcmt_reid`
(Eigen транзитивно). Он не линкует OpenCV, protobuf или Multitarget-tracker и
собирается в CI-конфигурации `EDGE=OFF, MTT=OFF, PROTOBUF=OFF`.

**CameraLinkModel.** Топология и статистика переходов читаются из YAML
(`configs/camera_links.yaml`, путь — `st.camera_links`):

```yaml
default_linked: false
fallback:
  min_transition_ms: 0
  max_transition_ms: 120000
cameras:
  cam-01:
    exit_zone:  [[east, north], ...]   # ENU-метры
  cam-02:
    entry_zone: [[east, north], ...]
links:
  - from: cam-01
    to: cam-02
    enabled: true
    mean_ms: 4000
    stddev_ms: 300
    samples: 42
```

- Зоны — полигоны в ENU-метрах в общем origin потока. Протокол не несёт размер
  кадра, поэтому пиксельные зоны невозможны без изменения протокола.
- `default_linked: false` — безопасный дефолт: без явного `links` никто не
  связан. `enabled: false` запрещает ребро даже при `default_linked: true`.
- Пустой `st.camera_links` = модель не загружена: link- и measured-transition
  проверки пропускаются, окно/зоны/BEV продолжают работать.
- Валидация: self-link, пустые id, дубликаты `(from, to)`, неполный набор
  `mean_ms/stddev_ms/samples`, `stddev_ms <= 0`, полигон < 3 точек, не-конечные
  координаты, `min_transition_ms >= max_transition_ms`.

**TransitionEstimator.** Робастная оценка по размеченным парам:

- группировка по направленному ребру `(from, to)`; отрицательные `Δt` и (по
  умолчанию) same-camera отбрасываются;
- `μ = median(Δt)`, `σ = 1.4826 · MAD`, нижний clamp `min_stddev_ms`;
- one-pass trim: точки дальше `outlier_sigma·σ` от медианы выбрасываются, если
  после trim остаётся ≥ `min_samples`;
- пары с числом валидных samples < `min_samples` в модель не попадают.

`apps/st_estimate` читает треклеты и JSONL `{"from": id, "to": id}`, берёт
`camera`/`begin`/`end` из самих треклетов и пишет YAML. `samples` — число
сэмплов после trim.

**SpatialGate.** Всегда проверяет same-camera (по умолчанию reject), link в
модели, `Δt ∈ [min_transition_ms, max_transition_ms]` и `Δt ∈ [μ−3σ, μ+3σ]` при
наличии статистики. В режиме `zone`: exit-точка источника в `exit_zone`,
entry-точка приёмника в `entry_zone`. В режиме `bev`: `d ≤ max_distance_m` и
`d ≤ v_max·Δt`. Отсутствие валидных world-координат → `MissingWorld`;
debug-флаг `allow_missing_world` пропускает zone/BEV-проверки.

**StReIdScorer.** Формулы — в §8.1. Без измеренной статистики `μ` и `σ` берутся
из fallback-окна: `μ = (min + max)/2`, `σ = (max − min)/6`.

**PairCandidateGenerator.** Берёт ANN-кандидатов через `ReIdMatcher`
(эффективный `top_k = top_k · oversample`), применяет гейты и `c_total`,
сортирует по возрастанию `c_total` (при равенстве — similarity desc, затем id)
и обрезает до `top_k`. `PairGenerationStats` считает причины отказов
(`unlinked`, `outside_window`, `outside_sigma`, `missing_world`, `exit_zone`,
`entry_zone`, `speed`, `distance`, `rejected_cost`, `truncated`).

**DoD Этапа 4.** Синтетика 5 камер × 50 объектов: precision ≥ 0.95,
recall ≥ 0.80, FP снижены ≥ 3× относительно appearance-only top-1. Реального
кросс-камерного GT для Auburn/Aire нет, поэтому precision/recall на этих
наборах не измеряются — проверка синтетическая.

### 8.6. Track-level метрики идентичности

`trafficmcmt_metrics` считает IDF1/IDP/IDR/IDSW на уровне треклетов, без
покадрового сопоставления. GT-идентичности и предсказанные `GlobalId`
сопоставляются венгерским алгоритмом по матрице совместных вхождений
треклетов:

```
IDTP = сумма выбранных ячеек
IDFP = predicted_tracks − IDTP
IDFN = gt_tracks − IDTP
IDP  = IDTP / (IDTP + IDFP)
IDR  = IDTP / (IDTP + IDFN)
IDF1 = 2·IDTP / (2·IDTP + IDFP + IDFN)
```

IDSW считается по каждой GT-траектории, упорядоченной по `begin` (при равенстве —
по `tracklet_id`): смена `GlobalId` между соседними сопоставленными треклетами.
Несопоставленные треклеты пропускаются, а не считаются переключением. Матрица
стоимости для Hungarian — `max_count − count`, поэтому максимизируется число
совпавших треклетов; пустые входы дают нули.

## 9. Нефункциональные требования

| Требование | Цель |
|---|---|
| Латентность online (ingest → association) | p99 ≤ 200 мс |
| Пропускная способность | 10 камер × 100 треклетов/с |
| Память индекса | ≤ 4 ГБ на 1M треклетов (int8 / IVF-PQ) |
| Решение flow | ≤ 2 с на 5k треклетов |
| Дрожание закоммиченных ID | ≤ 0.1% |
| Восстановление после рестарта брокера | ≤ 5 с |
| Потери при исправной сети | 0 |

## 10. Тестирование и деплой

См. `docs/roadmap.md` (этапы, DoD, тесты) и разделы 5–6 плана:

- Unit: GTest, формулы, гейты, буферы, кодек, ID-менеджер.
- Integration: in-process ZMQ, полный путь источник → синк.
- Golden/regression: IDF1/IDSW на фиксированных наборах.
- E2E: AI City Challenge (MTMC), Market-1501 (ReID), WildTrack (BEV).
- Деплой: `docker/master.Dockerfile` (CPU-only), `docker/edge.Dockerfile`,
  `docker/edge-jetson.Dockerfile`, `docker-compose.yml`.

## 11. Журнал решений (ADR)

| # | Решение | Обоснование | Альтернативы |
|---|---|---|---|
| ADR-001 | Master не линкует MTT/OpenCV | CPU-only деплой, независимость от Edge | линковать `MTTracking::mtracking` везде |
| ADR-002 | Protobuf, не FlatBuffers | зрелость, эволюция схемы, отладка; декодирование не узкое место | FlatBuffers при профилировании |
| ADR-003 | `IEmbeddingIndex` с 3 бэкендами | FAISS тяжело собирается на MSVC; нужен эталон для тестов | только FAISS |
| ADR-004 | `SspSolver` до muSSP | разблокирует Этап 5, даёт эталон оптимальности | ждать сборку muSSP |
| ADR-005 | ZMQ PUB/SUB + `sequence` | простота, фильтрация по топику; потери детектируются | gRPC streaming |
| ADR-006 | Скользящее окно + `commit_lag` | компромисс latency/качество, стабильные ID | полный offline, чистый online-greedy |
| ADR-007 | Геопривязка в `trafficmcmt_edge`, не в Master | Edge знает калибровку и кадры; Master получает готовые метры | проекция в Master по `CameraCalibration` |
| ADR-008 | ReID по типу объекта с отдельными `model_id` | пешеходы и транспорт требуют разных моделей; `model_id` защищает от смешивания | единая модель для всех типов |
| ADR-009 | HBEFA-таблица портирована в `trafficmcmt_emissions` с атрибуцией | расчёт сверяем с эталоном SUMO построчно, модуль не тянет GPL-код сборки | своя калибровка; отдельный GPL-модуль (см. `docs/emissions-attribution.md`) |
| ADR-010 | Галерея хранит авторитетные векторы, индекс — производная структура | точные `Remove`/`Evict`/`Rebuild` на любом бэкенде, включая приближённый FAISS | удалять только из индекса и доверять его `remove_ids` |
| ADR-011 | Зоны входа/выхода — ENU-метры, не пиксели; `default_linked: false` | общий origin уже есть у геопривязки, протокол не несёт размер кадра; явная топология аудируема и не пропускает случайные пары | пиксельные зоны (нужно менять протокол); `default_linked: true` |
| ADR-012 | Награда за связь `flow.link_reward` (λ=1.0) вычитается солвером, а не хранится в графе | граф остаётся независимым от решателя и хранит интерпретируемый `c_total`; отчётная стоимость не зависит от λ | хранить уже взвешенные рёбра (теряется сырая стоимость) |
| ADR-013 | muSSP подключается локальным форком через `TRAFFICMCMT_MUSSP_SOURCE_DIR`, без vendoring | не раздувает репозиторий и не требует установленного пакета; CI и обычная сборка остаются на `ssp`/`greedy` | vendoring исходников; обязательная установка muSSP |

## 12. Глоссарий

| Термин | Значение |
|---|---|
| **Треклет** | фрагмент траектории одного объекта на одной камере с эмбеддингом |
| **Late fusion** | объединение результатов на уровне треклетов, а не пикселей/детекций |
| **st-ReID** | spatio-temporal Re-identification: appearance + время + пространство |
| **BEV** | Bird's Eye View, вид сверху в мировых координатах (метры) |
| **MCF** | Minimum Cost Flow, глобальная ассоциация |
| **Global ID** | идентификатор объекта, сквозной для всех камер |
| **Watermark** | граница «всё, что старше — можно обрабатывать» в потоке с переупорядочиванием |
| **Puff** | порция выброса фиксированной массы, движущаяся с ветром и расширяющаяся по Гауссу |

## 13. Статус реализации

| Раздел | Статус |
|---|---|
| 1. Границы и правила | ✅ зафиксировано |
| 2. Окружение | ✅ проверено |
| 3. Топология | ✅ зафиксировано |
| 4. Потоки данных | ✅ зафиксировано |
| 5. Режимы | ✅ зафиксировано |
| 6. Модули/API | 🟡 `core`, `proto`, `io`, `edge` (ReID, geo, exporter), `reid`, `st`, `flow`, `metrics`, `map`, `net`, `emissions` реализованы; `pipeline`/`bev` — план |
| 7. Протокол | ✅ v1 + файловый I/O (JSONL / framed protobuf, int8); §7.5 — геопривязка, время, ReID по типу; §7.7 — `emission_grid` |
| 8. Алгоритмы | 🟡 `GeoBinding` (метры), ReID-индекс/галереи/матчер, st-ReID и MCF готовы; BEV — Этап 8 |
| 9. НФТ | 📋 измеряется с Этапа 6 |
| 10. Тесты/деплой | 🟡 251 unit-тест (протокол, geo, type-aware ReID, reid, st, flow, metrics, map, emissions, WebSocket), smoke на `aire` и Auburn; деплой — Этап 9 |
| 11. ADR | ✅ |
