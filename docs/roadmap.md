# TrafficMCMT — roadmap

> **Статус документа:** живая спецификация. Чекбоксы обновляются по мере выполнения.
> **Версия:** 0.3 · **Дата:** 2026-11-05
> **Оценки:** человеко-дни для одного senior C++ разработчика.

## Сводка

| # | Этап | Дней | Статус | Зависит от |
|---|---|---|---|---|
| 1 | Базовая инфраструктура | 3–4 | ✅ | — |
| 2 | Протокол и файловый I/O | 5–7 | ✅ | 1 |
| 2a | Тестовый пример: реальные видео | 2–3 | ✅ | 2 |
| 2b | Карта и viewer-фид (WebSocket v1) | 3–5 | ✅ | 2, 2a |
| 3 | FAISS и базовый ReID | 7–10 | ⬜ | 2 |
| 4 | st-ReID (Camera Link Model) | 8–12 | ⬜ | 2, 3 |
| 5 | muSSP и глобальная ассоциация | 10–14 | ⬜ | 3, 4 |
| 6 | ZeroMQ транспорт | 6–8 | ⬜ | 2 |
| 7 | Синхронизация времени | 7–10 | ⬜ | 5, 6 |
| 8 | BEV и мировые координаты | 8–12 | ⬜ | 4 |
| 9 | Прод-обвязка (опция) | 5–7 | ⬜ | 6, 7 |

**Итого:** 64–87 дней. Соло — 13–17 недель; два разработчика — 8–10 недель
(параллелятся: 3 ∥ 4, 6 ∥ 5, 8 ∥ 7).

Условные обозначения: ⬜ не начат · 🟡 в работе · ✅ готов · ⛔ заблокирован.

---

## Этап 1. Базовая инфраструктура (3–4 дня) ✅

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

## Этап 2a. Тестовый пример на реальных видео (2–3 дня) ✅

**Цель:** проверить Edge-часть на реальных данных: детектор YOLO/D-FINE, геопривязка,
ReID по типу объекта, две камеры с общей временной базой. Master pipeline (Этапы 3–5)
ещё не реализован, поэтому глобальные ID на этом этапе не проверяются.

### Задачи

- [x] `GeoBinding` — парсинг `geo_bind.ini`, WGS84→ENU, гомография, `Project`/`ToNormalizedPixel`
- [x] `TypeAwareReIdExtractor` — OSNet для пешеходов, HSV/DNN для транспорта, отдельные `model_id`
- [x] `DnnReIdExtractor` — ImageNet mean/std, per-channel нормализация
- [x] `apps/mtt_export`: `--geo-bind`, `--detector-settings`, `--white-list`,
      `--reid-person-model`, `--reid-vehicle-model`, `--reid-input`, `--start-epoch-ms`,
      `--start-frame`, `--local-dist`
- [x] Детектор через `BaseDetector::CreateDetector` (`YOLOV8`, `YOLO11`, `DFINE`)
- [x] `configs/detectors/coco_yolov8s.ini`, `configs/detectors/coco_dfine_s.ini`
- [x] `configs/datasets/aire.yaml`, `configs/datasets/auburn_toomer.yaml`
- [x] `scripts/run_aire.ps1`, `scripts/run_auburn_toomer.ps1`
- [x] Тесты `test_geo_binding.cpp`, `test_type_aware_reid.cpp`
- [x] Результаты пишутся в `build/datasets/...`; видео и модели вне git

### Прогресс (2026-11-03)

| Проверка | Результат |
|---|---|
| Сборка | `local-release`, MSVC 19.51 / VS 2026 — OK |
| Тесты | `ctest --preset local-release`: **84/84 passed** |
| `aire` (300 кадров) | 165 чанков, 4562 наблюдения, `world_valid=4562`, `reid_failures=0`; типы `car=141`, `truck=18`, `pedestrian=4`, `bus=2` |
| Auburn cam-01 (300 кадров с 1800) | 66 чанков, 1856 наблюдений, `world_valid=1856`; `pedestrian=40`, `car=14`, `truck=10`, `bike=2` |
| Auburn cam-02 (300 кадров с 1800) | 160 чанков, 4518 наблюдений, `world_valid=4518`; `pedestrian=40`, `car=112`, `bike=8` |
| Общая временная база | `[1620599100008, 1620599109937]` совпадает у обеих камер |
| Общий ENU origin | `--geo-origin 32.606456,-85.481891` для обеих камер; без него координаты сдвинуты на ~1.1 м |
| Эмбеддинги | 512-d, L2-норма = 1 у всех чанков |
| Межкамерные world-точки | первое 2-секундное окно: 199 совпавших timestamp, расстояние 0.53–2.80 м (медиана 1.62 м), 199/199 ≤ 5 м |
| `combined.jsonl` | 226 треклетов, 6374 наблюдения, 2 камеры |

### Команды

```powershell
cmake --build --preset local-release
powershell -ExecutionPolicy Bypass -File scripts/run_aire.ps1 -MaxFrames 300
powershell -ExecutionPolicy Bypass -File scripts/run_auburn_toomer.ps1 -MaxFrames 300
```

Полный прогон Auburn: `-Full` (10 204 кадра на камеру, ~340 с видео). Для двух камер
обязателен одинаковый `--start-epoch-ms` (по умолчанию `1620599040000`).

### Ограничения

- Master pipeline отсутствует: проверяется только Edge-экспорт и согласованность
  world-координат, а не глобальные ID.
- Для двух камер обязателен общий ENU origin (`-GeoOrigin` в скрипте); иначе
  координаты камер смещены друг относительно друга.
- Чёрный Jeep классифицируется как `truck` на cam-01 и `car` на cam-02 — межкамерная
  ассоциация должна учитывать `AddNearTypes(car, truck)`.
- Наивное сравнение nearest-neighbor по всем 300 кадрам включает объекты, видимые
  только в одной камере, поэтому метрика считается по 2-секундному окну с общими объектами.
- Короткие окна (300 кадров) выбраны для скорости; полный прогон — отдельная задача.
- `yolov8s.onnx` скопирован в локальный `Multitarget-tracker/data/coco`; модель в git не входит.

---

## Этап 2b. Карта и viewer-фид (3–5 дней) ✅

**Цель:** отдать обнаруженные объекты, их траектории и скорость на карту
`traffic-mcmt-viewer` по существующему протоколу v1, не втягивая MTT/OpenCV/CUDA
в Master. Master-часть работает с готовыми треклетами.

### Задачи

- [x] `TrackletChunk.origin_lat/origin_lon` (поля 13/14) + `Tracklet::HasOrigin()`,
      codec, `EdgeExportConfig`, `mtt_export`
- [x] `core/Geo.h` — единый WGS84↔ENU; `GeoBinding` делегирует туда
- [x] `trafficmcmt_map`: `MapConfig`, `MapFeed`, `MapIdRegistry`, `VelocityEstimator`,
      `TrailBuilder`, `TrackletTimeline`, `TrackletFiles`, `MapProtocol`
- [x] `trafficmcmt_net`: RFC 6455 сервер/клиент на Winsock/POSIX, `/health`
- [x] `apps/map_export` — offline: треклеты → JSONL-кадры v1
- [x] `apps/map_gateway` — WebSocket-реплей: зоны+snapshot на коннект, дельты,
      heartbeat, `--speed`, `--loop`, `--exit-on-end`, `--frames-out`
- [x] `configs/map_feed.yaml` (Auburn, 2 камеры), `configs/map_feed_aire.yaml`
- [x] Viewer: `speed_kmh`, `tracks_remove`, `TrackBuffer::remove`, отображение км/ч
- [x] `scripts/smoke_map_gateway.ps1` — smoke с реальным WebSocket-клиентом
- [x] Тесты `test_geo.cpp`, `test_map.cpp`, `test_ws.cpp`

### Прогресс (2026-11-05)

| Проверка | Результат |
|---|---|
| Сборка | `local-release`, MSVC 19.51 / VS 2026 — OK |
| Тесты C++ | `ctest --preset local-release`: **117/117 passed** |
| `map_export` Auburn | 226 треклетов, 6374 наблюдения → 107 кадров, `active_tracks=0` в конце |
| `map_gateway` offline | `--speed 0 --exit-on-end`: 1 `tracks_update` + 5 `tracks_remove` |
| `map_gateway` real-time | `--speed 2 --duration 2.5`: 49 `tracks_update` |
| WebSocket smoke | 43 кадра за 4 с: `camera_zones=1`, `tracks_update=42`, `max_speed_kmh=52.55` |
| Viewer | `vitest run`: **20/20**, `tsc --noEmit` — без ошибок |

### Команды

```powershell
cmake --build --preset local-release
ctest --preset local-release

.\build\local-release\Release\trafficmcmt_map_export.exe `
  --input build\datasets\auburn_toomer\combined.jsonl `
  --config configs\map_feed.yaml --out build\map\auburn_frames.jsonl

.\build\local-release\Release\trafficmcmt_map_gateway.exe `
  --input build\datasets\auburn_toomer\combined.jsonl `
  --config configs\map_feed.yaml --port 8080 --speed 1

powershell -ExecutionPolicy Bypass -File scripts/smoke_map_gateway.ps1
```

Viewer: `npm run dev` (по умолчанию `ws://localhost:8080/ws`) или `npm run mock`.

### Ограничения

- `map_gateway` пока реплеит файлы; живой приём из ZeroMQ — Этап 6.
- `origin` в конфиге должен совпадать с потоком; старые треклеты без origin
  требуют явного `feed.origin`.
- Зоны камер — выпуклая оболочка контрольных точек `geo_bind.ini`, а не точный
  полигон обзора.
- Скорость — мгновенная оценка (МНК 1 с); межкадровое сглаживание для UI не
  добавлялось.
- `tracks_remove` — аддитивное расширение; старые клиенты полагаются на TTL.

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
| 2026-11-03 | Добавлены `GeoBinding` (WGS84→ENU, гомография, RMS-проверка) и `TypeAwareReIdExtractor` (OSNet для пешеходов, HSV/DNN для транспорта); `mtt_export` получил геопривязку, детектор из `configs/detectors/*.ini`, `white_list` и общую временную базу |
| 2026-11-03 | `ctest` 84/84; `aire` 300 кадров: 165 чанков / 4562 наблюдения, `world_valid=100%`; Auburn 300 кадров с кадра 1800: cam-01 66 чанков / 1856 наблюдений, cam-02 160 чанков / 4518 наблюдений, временные диапазоны совпадают |
| 2026-11-03 | Добавлен общий ENU origin (`--geo-origin`, `origin_lat/origin_lon`); межкамерные world-координаты Auburn в 2-секундном окне: 199 совпавших timestamp, 0.53–2.80 м (медиана 1.62 м), все ≤ 5 м; добавлены `scripts/run_aire.ps1`, `scripts/run_auburn_toomer.ps1`, манифесты датасетов |
| 2026-11-05 | Этап 2b завершён: origin в протоколе, `core/Geo`, `trafficmcmt_map`, `trafficmcmt_net` (RFC 6455), `map_export`, `map_gateway`, `configs/map_feed*.yaml`; viewer получил `speed_kmh` и `tracks_remove`; `ctest` 117/117, `vitest` 20/20; smoke gateway: 43 кадра/4 с, скорость до 52.55 км/ч |
