# TrafficMCMT — roadmap

> **Статус документа:** живая спецификация. Чекбоксы обновляются по мере выполнения.
> **Версия:** 0.1 · **Дата:** 2026-11-02
> **Оценки:** человеко-дни для одного senior C++ разработчика.

## Сводка

| # | Этап | Дней | Статус | Зависит от |
|---|---|---|---|---|
| 1 | Базовая инфраструктура | 3–4 | 🟡 в работе | — |
| 2 | Протокол и файловый I/O | 5–7 | ⬜ | 1 |
| 3 | FAISS и базовый ReID | 7–10 | ⬜ | 2 |
| 4 | st-ReID (Camera Link Model) | 8–12 | ⬜ | 2, 3 |
| 5 | muSSP и глобальная ассоциация | 10–14 | ⬜ | 3, 4 |
| 6 | ZeroMQ транспорт | 6–8 | ⬜ | 2 |
| 7 | Синхронизация времени | 7–10 | ⬜ | 5, 6 |
| 8 | BEV и мировые координаты | 8–12 | ⬜ | 4 |
| 9 | Прод-обвязка (опция) | 5–7 | ⬜ | 6, 7 |

**Итого:** 59–79 дней. Соло — 12–16 недель; два разработчика — 7–9 недель
(параллелятся: 3 ∥ 4, 6 ∥ 5, 8 ∥ 7).

Условные обозначения: ⬜ не начат · 🟡 в работе · ✅ готов · ⛔ заблокирован.

---

## Этап 1. Базовая инфраструктура (3–4 дня) 🟡

**Цель:** собираемый каркас: CMake, конфиг, логирование, CLI, тесты, CI.
Публичный API ещё не содержит алгоритмов, но фиксирует доменные типы.

### Задачи

- [x] `CMakeLists.txt`: C++20, опции `TRAFFICMCMT_WITH_*`, `TRAFFICMCMT_BUILD_TESTS`
- [x] `cmake/Dependencies.cmake`: spdlog, yaml-cpp, GTest, MTTracking (+ FetchContent fallback)
- [x] `cmake/CompilerWarnings.cmake`: `/W4 /permissive- /utf-8` для MSVC, `-Wall -Wextra` для GCC/Clang
- [x] `CMakePresets.json` + `CMakeUserPresets.json` (локальные пути, gitignored)
- [x] `core/Types.h`, `core/Tracklet.h`, `core/Camera.h` — доменные типы
- [x] `core/Result.h` — `Result<T>` / `Status` без исключений в горячем пути
- [x] `core/Config.h` + `src/core/Config.cpp` — YAML-конфиг с валидацией и адресными ошибками
- [x] `core/Log.h` + `src/core/Log.cpp` — spdlog, консоль + rotating file
- [x] `apps/master_node/main.cpp` — CLI (`--config`, `--mode`, `--log-level`, `--dump-config`, `--version`)
- [x] `configs/master.yaml`, `configs/logging.yaml` — рабочие примеры
- [x] Unit-тесты: `Result`, парсинг/валидация конфига
- [x] `docs/architecture.md`, `docs/roadmap.md` — живые спецификации
- [x] `.clang-format`, обновлённый `.gitignore`, `README.md`
- [x] GitHub Actions: `ubuntu-latest` + `windows-latest` (сборка + ctest)
- [x] Проверка в CI, что `trafficmcmt_master` не тянет CUDA (`dumpbin /dependents` / `ldd`)

### Прогресс (2026-11-02)

| Проверка | Результат |
|---|---|
| Конфигурация | MSVC 19.51 (VS 2026), CMake 4.3.2 — OK |
| Сборка | `trafficmcmt_core`, `trafficmcmt_master`, `trafficmcmt_unit_tests` — OK |
| Тесты | `ctest`: **20/20 passed** (0.7 с) |
| Зависимости бинаря | только `KERNEL32`, `MSVCP140`, `VCRUNTIME140*`, `api-ms-win-crt-*` — OpenCV/CUDA/MTT отсутствуют |
| CLI | `--version`, `--help`, `--dump-config`, `--config`, `--log-config`, `--mode` — OK |
| Коды возврата | конфиг-ошибка → 1, ошибка аргументов → 2 |

Осталось: первый удалённый прогон CI (workflow готов, шаг проверки зависимостей добавлен).

### DoD

- `cmake --preset` + сборка проходят на MSVC и GCC/Clang.
- `trafficmcmt_master --help`, `--version`, `--dump-config` работают.
- Битый конфиг даёт ошибку с путём ключа (для ошибок типа/синтаксиса YAML — ещё строка), а не падение.
- Лог пишется в консоль и в файл; уровень управляется конфигом/CLI.
- `ctest` зелёный, unit-тесты < 5 с.
- Master-бинарь не линкует CUDA/TensorRT/OpenCV.

### Тесты

| Тест | Файл | Что проверяет |
|---|---|---|
| Result: ok/error, move, value_or | `tests/unit/test_result.cpp` | контракт `Result<T>` |
| Config: валидный YAML | `tests/unit/test_config.cpp` | все секции, значения по умолчанию |
| Config: невалидные значения | `tests/unit/test_config.cpp` | `window_ms=0`, `overlap>=window`, плохой enum |
| Config: отсутствует файл | `tests/unit/test_config.cpp` | `IoError` с путём |
| Config: битый YAML | `tests/unit/test_config.cpp` | `ParseError`, без исключений наружу |

### Артефакты

`trafficmcmt_core` (static), `trafficmcmt_master`, `tests/unit`, примеры конфигов.

---

## Этап 2. Протокол и файловый I/O (5–7 дней) ✅

**Цель:** видео → треклеты → файл → Master. Вся дальнейшая разработка идёт без камер.

### Задачи

- [x] `proto/tracklet.proto` v1 (`TrackletChunk`, `CameraStatus`, `Embedding`, `Observation`)
- [x] `proto/calibration.proto` v1 (`CameraCalibration`, `Polygon`)
- [x] `TrackletCodec` — protobuf ↔ домен, JSON-режим для отладки, int8-квантование
- [x] `FileTrackletSource` — JSONL + length-delimited protobuf, реплей с ускорением
- [x] `TrackletNormalizer` — инварианты протокола, dedup, L2-нормировка, quality
- [x] `InMemoryTrackletStore` — индексы по камере и времени
- [x] `JsonlSink` + `BinarySink`
- [x] `IReIdExtractor` + OpenCV DNN реализация (для Edge); HSV-гистограмма как fallback
- [x] `MttTrackAdapter`, `EdgeTrackExporter` (`trafficmcmt_mtt`)
- [x] `apps/mtt_export` — видео → `tracklets.jsonl`
- [x] `apps/tracklet_inspect` — чтение JSONL/protobuf, статистика, `--json`
- [ ] `apps/edge_simulator` — перенесён в Этап 6 (нужен только с ZMQ)

### Прогресс (2026-11-02)

| Проверка | Результат |
|---|---|
| MTT | локальный клон master в `C:\work\home\mtracker\TrafficMCMT\Multitarget-tracker`, собран MSVC 14.51.36231 (VS 2026) против OpenCV 4.12; install `...\Multitarget-tracker\install` |
| Конфигурация | OpenCV 4.12.0, MTTracking 1.1.0 (master, API `BaseTracker`), Protobuf 33.4.0; EDGE/MTT/PROTOBUF/TESTS=ON |
| Сборка | `trafficmcmt_core`, `_proto`, `_io`, `_edge`, `_mtt`, `_master`, `_mtt_export`, `_tracklet_inspect` — OK |
| Тесты | `ctest --preset local-release`: **69/69 passed** (1.8 с) |
| Smoke JSONL | `atrium.avi`, 300 кадров: 376 чанков, 8870 наблюдений, 0 ошибок ReID; результат совпал с прежней сборкой MTT |
| Smoke protobuf | 120 кадров + `--quantize`: 93 чанка, 2003 наблюдения; `embedding_norm ∈ [0.997, 1.002]` |
| Master | `dumpbin /dependents`: только Windows/MSVC runtime, без OpenCV/CUDA/MTT |

### DoD

- Round-trip `Tracklet → bytes → Tracklet` без потерь (допуск 1e-6 по float). ✅
- `mtt_export` на реальном видео даёт JSONL, читаемый `FileTrackletSource`. ✅
- 1 млн треклетов: загрузка < 30 с, RSS < 4 ГБ. ⏳ измерение вынесено в Этап 3
  (бенчмарк `BruteForceIndex`/`InMemoryTrackletStore`), датасета на 1M пока нет.
- Нормализатор отбраковывает битые сообщения и не падает (fuzz-тест). ✅
  Табличные кейсы + усечённые/битые framed-сообщения; отдельный fuzz-раннер — Этап 3.
- `schema_version` несовместимой версии отклоняется с внятной ошибкой. ✅

### Тесты

- Golden-файлы протокола (совместимость версий). ✅ `tests/data/tracklet_v1_golden.jsonl`
- Fuzz `Decode` (случайные байты, усечённые сообщения). 🟡 усечённые framed-сообщения покрыты; fuzz-раннер — Этап 3.
- Таблица кейсов нормализатора (пустой camera_id, begin > end, NaN в bbox, dim mismatch). ✅
- Выборки из store по временному диапазону и камере. ✅
- Интеграционный: `mtt_export` на 100 кадрах → чтение → проверка полей. ✅ smoke-прогон

---

## Этап 3. FAISS и базовый ReID (7–10 дней) ⬜

**Цель:** top-K похожих треклетов с других камер; измеримое качество ReID.

### Задачи

- [ ] `IEmbeddingIndex` (`Add`, `Search`, `Remove`, `Rebuild`)
- [ ] `BruteForceIndex` (Eigen) — эталон и дефолт для малых сессий
- [ ] `FaissIndex` — `FLAT_IP`, `IVF_PQ`, `HNSW32`; `nlist`/`nprobe` из конфига
- [ ] `EmbeddingNormalizer` — L2 + опциональный whitening/PCA (артефакт в файле)
- [ ] `GalleryManager` — галереи по камерам и временным окнам
- [ ] `ReIdMatcher` — пары «треклет → кандидаты» с `c_app`
- [ ] `apps/eval` (часть) — CMC/Rank-1/mAP
- [ ] `scripts/bench_index.cpp` — бенчмарк латентности

### DoD

- `BruteForceIndex` и `FaissIndex(FLAT)` дают идентичный top-K (recall@10 = 1.0).
- `IVF_PQ`: recall@10 ≥ 0.97 на 1M векторов.
- 1000 запросов по 1M × 512-d: p99 < 20 мс (CPU).
- Rank-1 на тестовом ReID-сплите ≥ 0.85 (фиксируется отчёт с моделью и препроцессингом).
- `TRAFFICMCMT_WITH_FAISS=OFF` собирается и проходит тесты на `BruteForceIndex`.

### Тесты

- Эквивалентность бэкендов, пустой индекс, dim mismatch, `Remove` + `Rebuild`.
- Детерминизм при одинаковом seed.
- Бенчмарк (Google Benchmark) в nightly.

---

## Этап 4. st-ReID и Camera Link Model (8–12 дней) ⬜

**Цель:** отсечь физически невозможные связи, снизить FP на порядок.

### Задачи

- [ ] `CameraLinkModel` — топология + `(μ, σ)` переходов, YAML I/O
- [ ] `TransitionEstimator` — робастная оценка `μ, σ` по данным (median/MAD)
- [ ] `SpatialGate` — zone-based (полигоны) и BEV-based (если есть H)
- [ ] `StReIdScorer` — fused cost + гейты
- [ ] `PairCandidateGenerator` — ANN + временные/пространственные гейты
- [ ] `configs/camera_links.yaml`

### DoD

- Синтетика (5 камер, 50 объектов, GT): precision связей ≥ 0.95, recall ≥ 0.80.
- FP относительно appearance-only снижается ≥ 3×.
- `TransitionEstimator` воспроизводит `μ, σ` с точностью ±15%.

### Тесты

- Формулы: монотонность, симметрия `c_app`, границы гейтов.
- Полигоны: внутри/снаружи/на границе.
- `TransitionEstimator` на синтетике с известными параметрами.
- Кейсы: камеры не связаны, `Δt` вне окна, объект стоял на месте.

---

## Этап 5. muSSP и глобальная ассоциация (10–14 дней) ⬜

**Цель:** непересекающиеся глобальные траектории, оптимальные по суммарной стоимости.

### Задачи

- [ ] `TrackletGraph` — split-узлы, ёмкости, рёбра с `c_total`
- [ ] `GraphPruner` — top-K, гейты, связные компоненты
- [ ] `IMcfSolver`, `SolverResult`
- [ ] `SspSolver` — собственный SSP (эталон + fallback)
- [ ] `MuSspSolver` — адаптер (`ToMuSspFormat` / `FromMuSspResult`)
- [ ] `FlowAssociator` — пути → `GlobalId`
- [ ] `GlobalIdManager` — union-find, `commit_lag`, merge-события
- [ ] `GreedyAssociator` — baseline

### DoD

- Графы ≤ 8 узлов: стоимость = полному перебору (для `MuSspSolver` и `SspSolver`).
- IDF1 на синтетическом MCMT ≥ 0.90.
- 5000 треклетов / 200k рёбер: решение < 2 с (CPU).
- Дрожание закоммиченных ID при пере-решении окна < 0.1%.
- `flow.solver=greedy|ssp` работает без muSSP.

### Тесты

- Построение графа: ёмкости, split, отсутствие рёбер при gate.
- Оптимальность против brute force.
- Несвязные компоненты, пустой граф.
- `GlobalIdManager`: merge, повторный merge, запрет переиспользования ID.
- Golden-регрессия IDF1.

---

## Этап 6. ZeroMQ транспорт (6–8 дней) ⬜

**Цель:** приём потоков от N камер в реальном времени.

### Задачи

- [ ] `ZmqContext` (RAII)
- [ ] `ZmqTrackletSource` (SUB, multipart, фильтр топика, reconnect)
- [ ] `ZmqTrackletPublisher` (Edge)
- [ ] `ZmqResultSink` (PUB)
- [ ] `MessageFramer` — версия, лимит размера, `[topic][payload]`
- [ ] `SequenceTracker` — пропуски → метрика
- [ ] `apps/edge_simulator` — N виртуальных камер
- [ ] `trafficmcmt_broker` (XSUB/XPUB), опционально

### DoD

- 10 камер × 100 треклетов/с, 1 час: `missing_total = 0` при исправной сети.
- p99 ingest → association < 200 мс.
- Рестарт брокера: восстановление < 5 с.
- RSS Master стабилен (±5%).

### Тесты

- In-process PUB/SUB на `tcp://127.0.0.1:*`.
- Отклонение чужой `schema_version`.
- Детект пропуска `sequence`.
- Нагрузка 10k msg/s.
- Chaos: рестарт брокера, обрыв линка.

---

## Этап 7. Синхронизация времени и out-of-order (7–10 дней) ⬜

**Цель:** корректность при рассинхронизации часов и переупорядочивании.

### Задачи

- [ ] `TimestampBuffer<T>` — watermark, `max_lateness`
- [ ] `ClockSyncEstimator` — offset (median) + drift (регрессия)
- [ ] `WindowScheduler` — окно, перекрытие, warm start
- [ ] `ReplayGuard` — поздние сообщения → пере-решение окна
- [ ] `MetricsCollector` — латентность, lateness, пропуски
- [ ] Онлайн-режим `MasterPipeline::RunOnline()`

### DoD

- Джиттер ±2 с и 5% переупорядочивания: IDF1 падает ≤ 2% относительно offline.
- Watermark не залипает при «молчащей» камере (закрытие по таймауту).
- Позднее сообщение обрабатывается в пределах одного окна.
- Побайтово детерминированный реплей.

### Тесты

- Все перестановки порядка для 4 элементов.
- Продвижение watermark, зависшая камера.
- Сходимость `ClockSyncEstimator` при дрейфе 100 ppm.
- Детерминизм `WindowScheduler`.
- Интеграция: 3 камеры с разными смещениями часов.

---

## Этап 8. BEV и мировые координаты (8–12 дней) ⬜

**Цель:** метры, дедупликация в зонах перекрытия, скорость/курс.

### Задачи

- [ ] `HomographyProjector` — H из калибровки / `findHomography` / HomLib
- [ ] `CameraCalibrationStore` — YAML/protobuf, версии, hot-reload
- [ ] `WorldTrajectoryFusion` — проекция, сглаживание, слияние, скорость
- [ ] `BeVSpatialGate` — расстояние в метрах
- [ ] `WorldTrackSink` — GeoJSON/CSV

### DoD

- Ошибка проекции на контрольных точках < 0.5 м.
- Подавление дублей в зоне перекрытия ≥ 90%.
- Скорость: точность ±10% на синтетике.
- Смена разрешения камеры не меняет решение st-ReID (метры, не пиксели).

### Тесты

- Round-trip `Project/Unproject` на синтетической H.
- Контрольные точки калибровки.
- Слияние двух перекрывающихся камер.
- Fallback на zone-based при отсутствии H.

---

## Этап 9 (опция). Прод-обвязка (5–7 дней) ⬜

- [ ] `docker/master.Dockerfile` (CPU-only), `docker/edge.Dockerfile`, `edge-jetson`
- [ ] `docker-compose.yml` (master + broker + edge_simulator + prometheus + grafana)
- [ ] Prometheus-метрики и `/healthz`, `/readyz`
- [ ] Graceful shutdown, ретеншн SQLite, бэкап
- [ ] ZMQ CURVE / изолированный VLAN, TLS для gRPC-фасада

---

## Метрики приёмки (сводно)

| Метрика | Синтетика | AI City | Этап |
|---|---|---|---|
| IDF1 (global) | ≥ 0.90 | ≥ 0.65 | 5 |
| IDP / IDR | ≥ 0.92 / 0.88 | ≥ 0.70 / 0.60 | 5 |
| ID switches | ≤ 1% | ≤ 5% | 5 |
| MOTA | ≥ 0.85 | ≥ 0.60 | 5 |
| HOTA | ≥ 0.75 | ≥ 0.50 | 5 |
| Rank-1 (ReID) | ≥ 0.95 | ≥ 0.85 (Market) | 3 |
| p99 ingest → assoc | ≤ 200 мс | — | 6 |
| Flow solve (5k) | ≤ 2 с | — | 5 |

## Реестр рисков (кратко)

| # | Риск | Этап | Митигация |
|---|---|---|---|
| 1 | Рассинхронизация часов | 7 | `ClockSyncEstimator`, `max_lateness`, NTP на Edge, алерт offset > 500 мс |
| 2 | Масштаб FAISS (1M+) | 3 | 3 бэкенда, int8, шардирование по камерам/времени, IVF-PQ |
| 3 | Взрыв графа muSSP | 5 | pruner, связные компоненты, лимит узлов, окна, `SspSolver` |
| 4 | Domain gap эмбеддингов | 2–3 | `model_id` в протоколе, единая модель, whitening, калибровочный прогон |
| 5 | Нестабильность ID | 5, 7 | `commit_lag`, merge-события, запрет переиспользования, метрика `id_merges` |
| 6 | Потери в сети | 6 | `sequence`, спулинг на Edge, reconnect, heartbeat |
| 7 | Сборка внешних зависимостей на MSVC | 2, 3, 5 | интерфейсы + рабочие дефолты (`BruteForce`, `SspSolver`), опции CMake |

## Журнал прогресса

| Дата | Событие |
|---|---|
| 2026-11-02 | Созданы `architecture.md`, `roadmap.md`; Этап 1: каркас CMake, `core/`, `master_node`, unit-тесты |
| 2026-11-02 | Сборка MSVC 2026 успешна; `ctest` 20/20; бинарь Master не линкует OpenCV/CUDA/MTT; добавлен CI workflow |
| 2026-11-02 | MTT пересобран против OpenCV 4.12 (vcpkg) с `USE_OCV_KCF/UKF/EMBEDDINGS=OFF`; ABI-конфликт с OpenCV 5.1 устранён |
| 2026-11-02 | Этап 2 завершён: protobuf v1, `TrackletCodec`, file source/sinks, normalizer, store, ReID, `mtt_export`, `tracklet_inspect`; `ctest` 69/69, golden-файл `tests/data/tracklet_v1_golden.jsonl` |
| 2026-11-02 | Smoke `mtt_export`: `atrium.avi` → 376 JSONL-чанков / 8870 наблюдений; protobuf + int8 → 93 чанка; Master по-прежнему CPU-only |
| 2026-11-02 | MTT склонирован в `C:\work\home\mtracker\TrafficMCMT\Multitarget-tracker` и собран MSVC 14.51.36231 (VS 2026); `mtt_export` адаптирован к API `BaseTracker` (`CreateTracker(settings, fps)`, `UpdateMat(..., time_point_t)`); 69/69, smoke 300 кадров дал те же 376 чанков / 8870 наблюдений |
