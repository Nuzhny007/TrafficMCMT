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
| `trafficmcmt_reid` | `IEmbeddingIndex`, `BruteForceIndex`, `FaissIndex`, `GalleryManager`, `ReIdMatcher` | FAISS (opt.), Eigen |
| `trafficmcmt_st` | `CameraLinkModel`, `TransitionEstimator`, `SpatialGate`, `StReIdScorer` | core, Eigen |
| `trafficmcmt_flow` | `TrackletGraph`, `GraphPruner`, `MuSspSolver`, `SspSolver`, `FlowAssociator`, `GlobalIdManager` | muSSP (opt.), Eigen |
| `trafficmcmt_bev` | `HomographyProjector`, `WorldTrajectoryFusion` | OpenCV core, Eigen |
| `trafficmcmt_net` | `ZmqTrackletSource`, `ZmqTrackletPublisher`, `ZmqResultSink` | libzmq/cppzmq, protobuf |
| `trafficmcmt_pipeline` | `MasterPipeline`, `WindowScheduler` | все выше |
| `trafficmcmt_mtt` | `MttTrackAdapter` | **MTTracking::mtracking**, OpenCV |
| `trafficmcmt_metrics` | `MotMetrics`, `Evaluator` | core |

Приложения Этапа 2: `trafficmcmt_mtt_export` (видео → треклеты; единственный таргет,
линкующий `MTTracking::mdetection`) и `trafficmcmt_tracklet_inspect` (статистика и
JSON-дамп треклетов).

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

## 8. Алгоритмические решения

### 8.1. Стоимость связи треклетов

```
c_app   = 1 - cos(e_a, e_b)                              ∈ [0, 2]
c_time  = clamp(-log N(Δt; μ_ab, σ_ab), 0, τ_t) / τ_t     ∈ [0, 1]
c_space = min(1, d_bev / (v_max · Δt))                   ∈ [0, 1]
c_total = (w_a·c_app + w_t·c_time + w_s·c_space) / (w_a + w_t + w_s)
```

Гейты (ребро не строится): `Δt ∉ [μ-3σ, μ+3σ]`, камеры не связаны в `CameraLinkModel`,
зоны выхода/входа не согласованы (zone-режим), `d_bev > v_max·Δt + ε` (BEV-режим).

### 8.2. Глобальная ассоциация

Граф: узел-треклет расщеплён на `in`/`out` с ёмкостью 1; рёбра `source→in`, `out→sink`,
`out_a→in_b` с весом `c_total`. Min-cost flow даёт набор непересекающихся путей —
каждый путь = глобальная траектория.

- `MuSspSolver` — адаптер к muSSP (основной).
- `SspSolver` — собственный successive shortest path (эталон корректности и fallback).
- `GreedyAssociator` — baseline для сравнения.
- `GraphPruner` — top-K кандидатов, гейты, разбиение на связные компоненты.

### 8.3. Стабильность ID

`GlobalIdManager`: union-find `TrackletId → GlobalId`; ID старше `commit_lag_ms`
замораживается; слияние оформляется `GlobalIdMergeEvent{old_id, new_id, reason, ts}`;
закоммиченный ID не переиспользуется.

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

## 13. Статус реализации

| Раздел | Статус |
|---|---|
| 1. Границы и правила | ✅ зафиксировано |
| 2. Окружение | ✅ проверено |
| 3. Топология | ✅ зафиксировано |
| 4. Потоки данных | ✅ зафиксировано |
| 5. Режимы | ✅ зафиксировано |
| 6. Модули/API | 🟡 `core`, `proto`, `io`, `edge` (ReID, geo, exporter), `mtt` реализованы; остальные — план |
| 7. Протокол | ✅ v1 + файловый I/O (JSONL / framed protobuf, int8); §7.5 — геопривязка, время, ReID по типу |
| 8. Алгоритмы | 🟡 `GeoBinding` (метры) готов; st-ReID и MCF — Этапы 3–5 |
| 9. НФТ | 📋 измеряется с Этапа 6 |
| 10. Тесты/деплой | 🟡 84 unit-теста (протокол, geo, type-aware ReID), smoke на `aire` и Auburn; деплой — Этап 9 |
| 11. ADR | ✅ |
