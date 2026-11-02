# TrafficMCMT — архитектура

> **Статус документа:** живая спецификация. Обновляется вместе с кодом.
> **Версия:** 0.1 (Этап 1)
> **Дата:** 2026-11-02

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
| Детекция объектов | `mdetection::BaseDetector` (YOLO / TensorRT / BGFG) |
| Локальный MOT | `mtracking::BaseTracker` (Kalman + Hungarian) |
| Локальный `track_id` | `TrackingObject::m_ID` |
| Извлечение ReID-эмбеддинга | `tmcmt::IReIdExtractor` (обвязка Edge, не MTT) |
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
| Multitarget-tracker (install) | `C:\work\ecology\Multitarget-tracker_rel\install` | 1.1.0 |
| CMake-конфиг MTT | `...\install\lib\MTTracking\cmake` | `MTTracking_DIR` |
| CMake-таргеты MTT | `MTTracking::mtracking`, `MTTracking::mdetection`, `MTTracking::inih` | `mtracking` → только `inih`; `mdetection` → CUDA/TensorRT |
| Include MTT | `MTTracking_INCLUDE_DIR` | интерфейсных include-путей у таргетов нет |
| OpenCV | `C:\work\libraries\opencv\5.0\build\win-install` | 5.0 + contrib, есть CUDA-сборка |
| spdlog | `C:\work\libraries\spdlog\rel\install` | `spdlog::spdlog` |
| yaml-cpp | `C:\work\libraries\yaml\yaml-cpp_build\install` | `yaml-cpp::yaml-cpp` |
| GTest | `C:\work\libraries\gtest\rel\install` | `GTest::gtest_main` |
| Eigen | `C:\work\libraries\eigen-3.4.0` | header-only |
| ZeroMQ / Protobuf / FAISS / muSSP | — | **не установлены**, подключаются на Этапах 2, 3, 5, 6 |

Следствия:

- Master собирается **CPU-only** и не линкует `MTTracking::mdetection`.
- `TrackingObject` не содержит эмбеддинга и timestamp → Edge-обвязка добавляет их сама.
- `mtracking/EmbeddingsCalculator.hpp` в установленных заголовках закрыт
  `#ifdef USE_OCV_EMBEDDINGS`, который потребителю не пробрасывается → Edge использует
  собственный `IReIdExtractor`.

### 2.1. Проверенная сборка

| Параметр | Значение |
|---|---|
| Компилятор | MSVC 19.51.36231 (Visual Studio 2026) |
| CMake | 4.3.2, генератор `Visual Studio 18 2026` |
| Конфигурация | `cmake --preset local-release` |
| Результат | `trafficmcmt_core.lib`, `trafficmcmt_master.exe`, `trafficmcmt_unit_tests.exe` |
| Тесты | `ctest --preset local-release` → 20/20 passed |
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
| `trafficmcmt_io` | `TrackletCodec`, `FileTrackletSource`, `JsonlSink`, `SqliteTrackletStore` | protobuf, sqlite3 |
| `trafficmcmt_reid` | `IEmbeddingIndex`, `BruteForceIndex`, `FaissIndex`, `GalleryManager`, `ReIdMatcher` | FAISS (opt.), Eigen |
| `trafficmcmt_st` | `CameraLinkModel`, `TransitionEstimator`, `SpatialGate`, `StReIdScorer` | core, Eigen |
| `trafficmcmt_flow` | `TrackletGraph`, `GraphPruner`, `MuSspSolver`, `SspSolver`, `FlowAssociator`, `GlobalIdManager` | muSSP (opt.), Eigen |
| `trafficmcmt_bev` | `HomographyProjector`, `WorldTrajectoryFusion` | OpenCV core, Eigen |
| `trafficmcmt_net` | `ZmqTrackletSource`, `ZmqTrackletPublisher`, `ZmqResultSink` | libzmq/cppzmq, protobuf |
| `trafficmcmt_pipeline` | `MasterPipeline`, `WindowScheduler` | все выше |
| `trafficmcmt_mtt` | `MttTrackAdapter`, `EdgeTrackExporter` | **MTTracking::mtracking**, OpenCV |
| `trafficmcmt_metrics` | `MotMetrics`, `Evaluator` | core |

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
| 6. Модули/API | 🟡 `core` реализован (Types, Tracklet, Camera, Config, Result, Log); остальные — план |
| 7. Протокол | 📋 Этап 2 |
| 8. Алгоритмы | 📋 Этапы 3–5 |
| 9. НФТ | 📋 измеряется с Этапа 6 |
| 10. Тесты/деплой | 🟡 20 unit-тестов `core`, CI workflow; деплой — Этап 9 |
| 11. ADR | ✅ |
