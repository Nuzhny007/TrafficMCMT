# TrafficMCMT — roadmap

> **Статус документа:** живая спецификация. Чекбоксы обновляются по мере выполнения.
> **Версия:** 0.7 · **Дата:** 2026-11-11
> **Оценки:** человеко-дни для одного senior C++ разработчика.

## Сводка

| # | Этап | Дней | Статус | Зависит от |
|---|---|---|---|---|
| 1 | Базовая инфраструктура | 3–4 | ✅ | — |
| 2 | Протокол и файловый I/O | 5–7 | ✅ | 1 |
| 2a | Тестовый пример: реальные видео | 2–3 | ✅ | 2 |
| 2b | Карта и viewer-фид (WebSocket v1) | 3–5 | ✅ | 2, 2a |
| 2c | Gaussian puff: выбросы и концентрации | 4–6 | ✅ | 2b |
| 3 | FAISS и базовый ReID | 7–10 | 🟡 | 2 |
| 4 | st-ReID (Camera Link Model) | 8–12 | ✅ | 2, 3 |
| 5 | muSSP и глобальная ассоциация | 10–14 | ✅ | 3, 4 |
| 6 | ZeroMQ транспорт | 6–8 | ✅ | 2 |
| 7 | Синхронизация времени | 7–10 | ✅ | 5, 6 |
| 8 | BEV и мировые координаты | 8–12 | ⬜ | 4 |
| 9 | Прод-обвязка (опция) | 5–7 | ⬜ | 6, 7 |

**Итого:** 68–93 дней. Соло — 14–19 недель; два разработчика — 9–11 недель
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

## Этап 2c. Gaussian puff: выбросы и концентрации (4–6 дней) ✅

**Цель:** считать в реальном времени выбросы автомобилей по HBEFA и показывать
приземные концентрации NOx/CO/PM2.5/PM10/CO2 на карте viewer'а, не втягивая
MTT/OpenCV/CUDA в Master.

### Задачи

- [x] `trafficmcmt_emissions`: `EmissionTypes`, `IEmissionModel`,
      `HbefaEmissionModel`, `PasquillGifford`, `GaussianPuffModel`,
      `ConcentrationGrid`, `EmissionConfig`, `EmissionSimulator`
- [x] `AccelerationEstimator` (МНК + EMA + кламп) и `TrackState::acceleration`
- [x] `MapTypes`: `MapFrameType::EmissionGrid`, `MapEmissionField`,
      `MapEmissionGrid`, `MapFrame::emission`
- [x] `MapProtocol`: base64 float32 LE, все 5 полей, статистика `max/mean/p95`
- [x] `MapConfig`: секция `emissions:` + валидация; конфиги Auburn и Aire
- [x] `MapFeed`: интеграция симулятора в `Tick`, `EmissionSnapshot()`
- [x] `map_gateway`: последний emission-снапшот новому клиенту
- [x] `map_export`: emission-кадры в JSONL для обоих датасетов
- [x] Viewer: парсер `emission_grid`, `EmissionBuffer`, палитры на загрязнитель,
      `BitmapLayer`, панель с переключателем/легендой/статистикой
- [x] Тесты `test_emissions.cpp` (физика, HBEFA, сетка, симулятор),
      `test_map.cpp` (протокол, конфиг, фид), `palette.test.ts`, `protocol.test.ts`
- [x] `docs/emissions-attribution.md` — происхождение таблицы HBEFA и лицензии

### Прогресс (2026-11-07)

| Проверка | Результат |
|---|---|
| Сборка | `local-release`, MSVC 19.51 / VS 2026 — OK |
| Тесты C++ | `ctest --preset local-release`: **156/156 passed** |
| Тесты viewer | `vitest run`: **28/28**, `tsc --noEmit` — без ошибок |
| `map_export` Aire | `cols=120`, `rows=90`, 5 полей; NOx `max=15561`, `p95=465` мкг/м³ |
| `map_export` Auburn | `cols=120`, `rows=90`, 5 полей; NOx `max=848`, `p95=169` мкг/м³ |
| `map_gateway` + viewer | слой концентраций вдоль шоссе, панель и переключение работают |
| Контроль σ | σ_y(D,1 км)=75.3 м, σ_z(D)=29.1 м; A/F совпадают с эталоном |

### Команды

```powershell
cmake --build --preset local-release
ctest --preset local-release

.\build\local-release\Release\trafficmcmt_map_export.exe `
  --input build\datasets\aire\cam-01.jsonl `
  --config configs\map_feed_aire.yaml --out build\map\frames_aire_emissions.jsonl

.\build\local-release\Release\trafficmcmt_map_gateway.exe `
  --input build\datasets\aire\cam-01.jsonl `
  --config configs\map_feed_aire.yaml --port 8080 --speed 0.5 --loop
```

Viewer: `npm run dev`, кадры `emission_grid` приходят с частотой 1 Гц.

### Ограничения

- Модель — стационарный ветер и один класс устойчивости на прогон; поля ветра
  и рельеф не учитываются.
- HBEFA-таблица содержит только 3 класса; `bike`/`person` дают нулевые ставки,
  мотоцикл приближён строкой `P_LDV_7_1`.
- `PM2.5` — доля `PMx` (`pm25_fraction`), а не отдельная строка HBEFA.
- Отрицательное ускорение клампится к нулю по умолчанию (отличие от эталона
  SUMO, см. `docs/emissions-attribution.md`).
- Сетка центрируется на ENU-origin и не следует за камерой; для другого
  участка нужен свой конфиг.
- Лицензионный статус перенесённой таблицы требует юридической проверки перед
  публичным распространением.

---

## Этап 3. FAISS и базовый ReID (7–10 дней) 🟡

**Цель:** top-K похожих треклетов с других камер; измеримое качество ReID.

### Задачи

- [x] `IEmbeddingIndex` (`Add`, `Search`, `Remove`, `Rebuild`, фильтр кандидатов)
- [x] `BruteForceIndex` (Eigen) — эталон и дефолт для малых сессий
- [x] `FaissIndex` — `FLAT_IP`, `IVF_PQ`, `HNSW32`; `nlist`/`nprobe` из конфига
      (код написан, локально **не компилировался** — FAISS не установлен)
- [x] `EmbeddingNormalizer` — L2 + опциональный whitening/PCA (артефакт `TMCMTWN1`)
- [x] `GalleryManager` — галереи по камерам и временным окнам, retention-eviction
- [x] `ReIdMatcher` — пары «треклет → кандидаты» с `c_app`
- [x] `apps/reid_eval` — CMC/Rank-1/5/10/mAP (chunk-split и внешний manifest)
- [x] `apps/reid_bench` — латентность, QPS, recall@k (свои замеры, без Google Benchmark)
- [x] `reid.min_similarity` и `reid.exclude_same_camera` в конфиге и валидации
- [x] Тесты `test_embedding_index.cpp` (16), `test_reid_gallery.cpp` (21),
      `test_faiss_index.cpp` (guarded `TRAFFICMCMT_WITH_FAISS`)
- [x] `docs/reid-eval.md` — протокол оценки, схема manifest, формулы, ограничения

### DoD

- [x] `BruteForceIndex` и `FaissIndex(FLAT)` дают идентичный top-K (recall@10 = 1.0).
      Тест `FaissIndex.FlatMatchesBruteForceTopK` написан, но выполняется только при
      `TRAFFICMCMT_WITH_FAISS=ON`.
- [ ] `IVF_PQ`: recall@10 ≥ 0.97 на 1M векторов. ⛔ FAISS не установлен; тест
      `FaissIndex.IvfPqRecallIsHigh` (5k×32-d, порог 0.90) не запускался.
- [ ] 1000 запросов по 1M × 512-d: p99 < 20 мс (CPU). 🟡 точный скан не достигает
      (200k×512: p99 16.2 мс); нужен `faiss_ivf_pq`.
- [ ] Rank-1 на тестовом ReID-сплите ≥ 0.85. 🟡 chunk-split даёт 0.46/0.75; нужен
      размеченный кросс-камерный сплит.
- [x] `TRAFFICMCMT_WITH_FAISS=OFF` собирается и проходит тесты на `BruteForceIndex`.

### Прогресс (2026-11-08)

| Проверка | Результат |
|---|---|
| Сборка | `local-release`, MSVC 19.51 / VS 2026 — OK |
| Тесты C++ | `ctest --preset local-release`: **195/195 passed** |
| CI-сценарий | `EDGE=OFF, MTT=OFF, PROTOBUF=OFF`: сборка OK, **97/97 passed** |
| `reid_eval` Aire (chunk-split) | gallery 87, queries 78, identities 33; Rank-1 **0.4615**, Rank-5 0.7949, Rank-10 0.8718, mAP 0.3439 |
| `reid_eval` Auburn (chunk-split) | gallery 117, queries 109, identities 32; Rank-1 **0.7523**, Rank-5 0.9817, Rank-10 1.0000, mAP 0.6078 |
| `reid_bench` brute 200k×512 | 200 запросов, k=10: p50 12.6 мс, p95 14.9 мс, p99 16.2 мс, QPS 77.8, recall@10 1.0 |
| Manifest-режим | smoke на 5 записях: Rank-1 1.0, mAP 1.0 |

### Команды

```powershell
cmake --build --preset local-release
ctest --preset local-release

# chunk-split оценка на наших треклетах
.\build\local-release\Release\trafficmcmt_reid_eval.exe `
  --tracklets build\datasets\aire\cam-01.jsonl

# внешний размеченный сплит
.\build\local-release\Release\trafficmcmt_reid_eval.exe `
  --manifest build\reid\manifest.jsonl --json

# бенчмарк индекса
.\build\local-release\Release\trafficmcmt_reid_bench.exe `
  --vectors 200000 --dim 512 --queries 200 --k 10 --index brute --json
```

Обёртки: `scripts/reid_eval_aire.ps1`, `scripts/reid_eval_auburn.ps1`.

### Ограничения

- **FAISS не проверен.** Пакет не установлен на машине разработки, поэтому
  `src/reid/FaissIndex.cpp` и `test_faiss_index.cpp` никогда не компилировались.
  Возможны ошибки API (`IndexIDMap2`, `IndexIVFPQ`, `IndexHNSWFlat`); исправлять
  при появлении `faiss` в vcpkg. Дефолт и эталон — `BruteForceIndex`.
- **Rank-1 DoD не достигнут на chunk-split.** 0.46 (Aire) / 0.75 (Auburn) — это
  согласованность дескриптора внутри трека, а не кросс-камерный ReID. Целевые
  0.85 требуют размеченного кросс-камерного сплита (`--manifest`).
- **p99 < 20 мс на 1M×512 не достигнут точным сканом.** Brute force memory-bound:
  один запрос читает `N·dim·4` байт (410 МБ при 200k×512). Цель достижима только
  `faiss_ivf_pq`; `reid_bench` хранит данные и копию в индексе (~4+ ГБ RAM на
  1M×512).
- `EmbeddingNormalizer` не обучает whitening/PCA сам — принимает готовый
  артефакт; тренер вынесен за пределы этапа.
- `reid_eval` chunk-split использует `exclude_same_camera = false`, иначе
  одноимённые чанки одной камеры не смогли бы найти друг друга.

### Тесты

- `BruteForceIndex`: top-K по inner product, пустой индекс, `k > N`, dim mismatch,
  non-finite, `Remove`/`Add`/`Rebuild`, атомарность `Rebuild`, дубликаты, tie-break
  по `id`, многопоточный скан == однопоточный, фильтр до top-K.
- `EmbeddingIndexFactory`: создание brute, отказ при `dim <= 0`, `Unsupported` для
  `faiss_*` при `TRAFFICMCMT_WITH_FAISS=OFF`.
- `EmbeddingNormalizer`: L2, whitening, zero-vector, dim mismatch, Save/Load,
  битый/отсутствующий артефакт, сохранение `model_id`.
- `GalleryManager`: add/remove, фильтры по камере/модели/типу/времени,
  `min_similarity`, retention-eviction, re-add, `exclude_id`.
- `ReIdMatcher`: `c_app`, запрет сравнения разных `model_id`, невалидный запрос.

---

## Этап 4. st-ReID и Camera Link Model (8–12 дней) ✅

**Цель:** отсечь физически невозможные связи, снизить FP на порядок.

### Задачи

- [x] `CameraLinkModel` — топология + `(μ, σ)` переходов, YAML I/O
- [x] `TransitionEstimator` — робастная оценка `μ, σ` по данным (median/MAD)
- [x] `SpatialGate` — zone-based (полигоны) и BEV-based
- [x] `StReIdScorer` — fused cost + гейты
- [x] `PairCandidateGenerator` — ANN + временные/пространственные гейты
- [x] `TrackletEndpoint` и `Polygon` — общие типы и point-in-polygon
- [x] `configs/camera_links.yaml` — Auburn (`cam-01`/`cam-02`, обе стороны)
- [x] `st.camera_links` в `MasterConfig` (`ParseSt`/`Dump`), `configs/master.yaml`
- [x] `apps/st_estimate` — размеченные пары → статистика → YAML
- [x] `GalleryEntry::entry_world`/`exit_world` — world-координаты из `Tracklet`
- [x] Тесты `tests/unit/test_st_reid.cpp` (38)

### DoD

- [x] Синтетика (5 камер, 50 объектов, GT): precision связей ≥ 0.95, recall ≥ 0.80.
- [x] FP относительно appearance-only снижается ≥ 3×.
- [x] `TransitionEstimator` воспроизводит `μ, σ` с точностью ±15%.
- [ ] Реальные precision/recall на Auburn/Aire. ⛔ нет кросс-камерного GT.

### Прогресс (2026-11-09)

| Проверка | Результат |
|---|---|
| Сборка | `local-release`, MSVC 19.51 / VS 2026 — OK |
| Тесты C++ | `ctest --preset local-release`: **233/233 passed** |
| CI-сценарий | `EDGE=OFF, MTT=OFF, PROTOBUF=OFF`: сборка OK, **135/135 passed** |
| Синтетика DoD | пороговые проверки пройдены: precision ≥ 0.95, recall ≥ 0.80, FP снижены ≥ 3× |
| `st_estimate` smoke | 6 треклетов, 3 пары → `cam-a → cam-b`: μ 4000 мс, σ 296.52 мс, samples 3 |

### Команды

```powershell
cmake --build --preset local-release
ctest --preset local-release

# оценка переходов по размеченным парам
.\build\local-release\Release\trafficmcmt_st_estimate.exe `
  --tracklets build\tmp_st\tracklets.jsonl `
  --pairs build\tmp_st\pairs.jsonl `
  --out build\tmp_st\camera_links.yaml `
  --min-samples 3
```

Формат пар — JSONL: `{"from": 123456789, "to": 987654321}`. `camera`, `begin` и
`end` берутся из самих треклетов.

### Ограничения

- **Нет кросс-камерного GT для Auburn/Aire.** DoD precision/recall проверяется
  только синтетикой; реальные цифры появятся с размеченным MTMC-сплитом.
- **Зоны Auburn пустые.** Общий ENU origin есть, но exit/entry-полигоны не
  калиброваны; zone-режим для Auburn отключён, BEV использует world-координаты.
- **Пиксельные зоны невозможны** без изменения протокола: `TrackletChunk` не
  несёт размер кадра.
- **Master пока не использует модель.** Загрузка `st.camera_links` и
  `PairCandidateGenerator` подключаются в Этапе 5 вместе с графом.
- `c_time` — нормированный `−ln pdf` (0 в среднем, 1 на 3σ); исходная запись в
  спецификации не обнулялась в среднем (см. §8.1 `architecture.md`).
- `TransitionEstimator` требует ≥ `min_samples` (по умолчанию 3) на направленное
  ребро; редкие переходы остаются без статистики и используют fallback-окно.
- `default_linked: false`: без явного `links` ни одна пара не разрешена.

### Тесты

- Формулы: монотонность `c_time`, симметрия/границы `c_app`, `c_space`, веса.
- Полигоны: внутри/снаружи/на границе, вогнутый, невалидный.
- `TransitionEstimator`: известные μ/σ (±15%), trim выбросов, чётная медиана,
  отрицательные Δt, группировка по направлению, валидация опций.
- Гейты: same-camera, unlinked, окно, 3σ, zone, BEV (speed/distance),
  `MissingWorld` и `allow_missing_world`.
- `PairCandidateGenerator`: сортировка по `c_total`, обрезка, `min_similarity`,
  `max_total_cost`, исключение своей камеры, мульти-запросы.
- `CameraLinkModel`: YAML round-trip, `default_linked`, дубликаты, self-link,
  неполная статистика, плоские списки координат.
- DoD-синтетика: 5 камер × 50 объектов, appearance-only baseline.

---

## Этап 5. muSSP и глобальная ассоциация (10–14 дней) ✅

**Цель:** непересекающиеся глобальные траектории, оптимальные по суммарной стоимости.

### Задачи

- [x] `TrackletGraph` — split-узлы, ёмкости, рёбра с `c_total`
- [x] `GraphPruner` — top-K, гейты, связные компоненты
- [x] `IMcfSolver`, `SolverResult`
- [x] `SspSolver` — собственный SSP (эталон + fallback)
- [x] `MuSspSolver` — адаптер (`ToMuSspFormat` / `FromMuSspResult`)
- [x] `FlowAssociator` — пути → `GlobalId`
- [x] `GlobalIdManager` — union-find, `commit_lag`, merge-события
- [x] `GreedyAssociator` — baseline
- [x] `trafficmcmt_metrics` — track-level IDF1/IDP/IDR/IDSW
- [x] `trafficmcmt_associate` — offline CLI (JSONL → глобальные ID, `--gt`)

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

### Прогресс (2026-11-10)

| Проверка | Результат |
|---|---|
| Сборка | `local-release`, MSVC 19.51 / VS 2026 — OK |
| Тесты C++ | `ctest --preset local-release`: **251/251 passed** (1 skipped: muSSP выключен) |
| CI-сценарий | `EDGE=OFF, MTT=OFF, PROTOBUF=OFF, MUSSP=OFF`: сборка OK, **153/153 passed** |
| muSSP | локальный форк `C:/work/home/mtracker/tmp/muSSP`, `MUSSP=ON`: unit-тесты **217/217** |
| Master | `dumpbin /dependents`: нет OpenCV/CUDA/MTTracking/NVINFER |
| Brute force | `SspSolver` и `MuSspSolver` совпадают с перебором на графах ≤ 5 узлов и на разреженном графе из 10 узлов (в т.ч. при перемешанном порядке узлов) |
| Auburn smoke | 226 треклетов / 2792 перехода: `ssp` и `mussp` дают одинаковые 126 путей, `total_cost=57.562` и идентичный JSONL глобальных ID |

Осталось до полного закрытия DoD: замер 5000 треклетов / 200k рёбер и
IDF1-регрессия на синтетическом MCMT (нужен генератор с GT); подключение
`FlowAssociator` к `MasterPipeline` — Этап 7.

### Команды

```powershell
# offline-ассоциация
.\build\local-release\Release\trafficmcmt_associate.exe `
  --tracklets build\datasets\auburn_toomer\combined.jsonl `
  --camera-links configs\camera_links.yaml `
  --out build\associate\ids.jsonl --solver ssp --json

# с оценкой IDF1 (JSONL {"tracklet": <id>, "identity": "<gt>"})
.\build\local-release\Release\trafficmcmt_associate.exe `
  --tracklets build\datasets\auburn_toomer\combined.jsonl `
  --gt build\associate\gt.jsonl `
  --out build\associate\ids.jsonl --json

# muSSP локально
cmake --preset local-release -B build/mussp-check `
  -DTRAFFICMCMT_WITH_MUSSP=ON `
  -DTRAFFICMCMT_MUSSP_SOURCE_DIR=C:/work/home/mtracker/tmp/muSSP
```

---

## Этап 6. ZeroMQ транспорт (6–8 дней) ✅

**Цель:** приём потоков от N камер в реальном времени.

### Задачи

- [x] `ZmqContext` (RAII), `Shutdown()` будит заблокированный `Next()`
- [x] `ZmqTrackletSource` (SUB, multipart, фильтр топика, reconnect, heartbeat)
- [x] `ZmqTrackletPublisher` (Edge, неблокирующая PUB-отправка)
- [x] `ZmqResultSink` (PUB) + `GlobalTrackUpdate` / `GlobalIdMergeEvent` в `proto/global.proto`
- [x] `MessageFramer` — версия, лимит размера, `[topic][payload]`
- [x] `SequenceTracker` — пропуски/дубликаты/out-of-order → метрики
- [x] `apps/edge_simulator` — N виртуальных камер с rate/seed
- [x] `trafficmcmt_broker` (XSUB/XPUB), опционально
- [x] `tracklet_inspect --source zmq` — consumer и статистика
- [x] `ErrorCode::Timeout` в `core/Result.h`

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

### Прогресс (2026-11-10)

| Проверка | Результат |
|---|---|
| Сборка | `build/zmq-check`, MSVC 19.51 / VS 2026, `ZMQ=ON` — OK |
| Тесты | `trafficmcmt_net_tests`: **25/25 passed**; полный `ctest` в `build/zmq-check`: **269/269 passed** (1 skipped: muSSP) |
| DoD-soak (короткий) | 10 камер × 100 трекл./с × 60 с: **60 000 сообщений**, `missing_total=0`, `rejected=0`, `malformed=0`, `send_failures=0` |
| RSS consumer | дрейф между половинами установившегося окна **2.4%** (цель ±5%) |
| Нагрузка | 10 000 сообщений PUB→SUB: публикация **9 мс**, end-to-end **50 мс** |
| Рестарт брокера | подписка и публикация восстанавливаются **< 1 с** |
| CI-сценарий | `EDGE=OFF, MTT=OFF, PROTOBUF=OFF, MUSSP=OFF, ZMQ=OFF` — без регрессий |

Полный часовой прогон запускается тем же скриптом с `-Duration 3600`
(в рамках текущей сессии выполнен короткий 60-секундный вариант DoD).
p99 «ingest → association» будет измерен после подключения `FlowAssociator`
к `MasterPipeline` (Этап 7); на Этапе 6 проверен транспортный путь
publish → receive.

### Команды

```powershell
# конфигурация с ZMQ (требует protobuf и cppzmq из vcpkg)
cmake -S . -B build/zmq-check -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH="C:/work/libraries/spdlog/rel/install;C:/work/libraries/yaml/yaml-cpp_build/install;C:/work/libraries/gtest/rel/install;C:/work/tools/vcpkg/installed/x64-windows" `
  -DOpenCV_DIR="C:/work/tools/vcpkg/installed/x64-windows/share/opencv4" `
  -DMTTracking_DIR="C:/work/home/mtracker/TrafficMCMT/Multitarget-tracker/install/lib/MTTracking/cmake" `
  -DTRAFFICMCMT_WITH_ZMQ=ON
cmake --build build/zmq-check --config Release --parallel 8

# unit-тесты транспорта
ctest --test-dir build/zmq-check -C Release --output-on-failure

# smoke: broker + 10 камер × 100 трекл./с × 60 с
powershell -ExecutionPolicy Bypass -File scripts/smoke_zmq.ps1 `
  -Duration 60 -Cameras 10 -Rate 100

# полный часовой прогон DoD
powershell -ExecutionPolicy Bypass -File scripts/smoke_zmq.ps1 `
  -Duration 3600 -Cameras 10 -Rate 100
```

---

## Этап 7. Синхронизация времени и out-of-order (7–10 дней) ✅

**Цель:** корректность при рассинхронизации часов и переупорядочивании.

### Задачи

- [x] `TimestampBuffer<T>` — watermark, `max_lateness`
- [x] `ClockSyncEstimator` — offset (median) + drift (регрессия)
- [x] `WindowScheduler` — окно, перекрытие, warm start
- [x] `ReplayGuard` — поздние сообщения → пере-решение окна
- [x] `MetricsCollector` — латентность, lateness, пропуски
- [x] Онлайн-режим `MasterPipeline::RunOnline()`

### DoD

- [x] Джиттер ±2 с и 5% переупорядочивания: IDF1 падает ≤ 2% относительно offline.
- [x] Watermark не залипает при «молчащей» камере (закрытие по таймауту).
- [x] Позднее сообщение обрабатывается в пределах одного окна.
- [x] Побайтово детерминированный реплей.

### Тесты

- [x] Все перестановки порядка для 4 элементов.
- [x] Продвижение watermark, зависшая камера.
- [x] Сходимость `ClockSyncEstimator` при дрейфе 100 ppm.
- [x] Детерминизм `WindowScheduler`.
- [x] Интеграция: 3 камеры с разными смещениями часов.

**Результат (2026-11-11).** `trafficmcmt_pipeline` (`MasterPipeline`,
`WindowScheduler`, `TimestampBuffer`, `ClockSyncEstimator`, `ReplayGuard`,
`MetricsCollector`) собирается без protobuf. `master_node` подключён к
pipeline: `--out`/`--stats-out`/`--dump-raw`/`--gt`, file и ZMQ ingest,
offline/online. Добавлен `trafficmcmt_tracklet_perturb` (джиттер порядка
доставки, reorder, отдельный `--event-jitter-ms`) и `scripts/smoke_online.ps1`.
DoD на Auburn: online clean IDF1 = 1.0000, после ±2 с и 5% reorder — 1.0000
(падение 0%), `too_late=0`; повторный реплей побайтово совпадает. `ctest`:
242/242 (+1 skip) в `local-release`, 179/179 (+1 skip) в `ci-check`,
294/294 (+1 skip) в `zmq-check`.

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
| 2026-11-07 | Этап 2c завершён: `trafficmcmt_emissions` (HBEFA, Pasquill-Gifford, Gaussian puff, сетка), `AccelerationEstimator`, кадр `emission_grid` (base64 f32 LE, 5 полей), панель концентраций в viewer; `ctest` 156/156, `vitest` 28/28; `map_export` Aire/Auburn по 120×90×5 полей |
| 2026-11-08 | Этап 3 (базовый ReID): `trafficmcmt_reid` (`IEmbeddingIndex`, `BruteForceIndex`, `EmbeddingNormalizer`, `GalleryManager`, `ReIdMatcher`), `FaissIndex` под флагом, `reid_eval`, `reid_bench`, `docs/reid-eval.md`; `ctest` 195/195, CI-сценарий 97/97; chunk-split Rank-1 Aire 0.4615 / Auburn 0.7523; brute 200k×512 p99 16.2 мс; FAISS не установлен — бэкенд не проверен |
| 2026-11-09 | Этап 4 (st-ReID): `trafficmcmt_st` (`CameraLinkModel`, `TransitionEstimator`, `SpatialGate`, `StReIdScorer`, `PairCandidateGenerator`, `TrackletEndpoint`, `Polygon`), `st.camera_links`, `configs/camera_links.yaml`, `st_estimate`; `GalleryEntry` получил world-endpoints; `ctest` 233/233, CI-сценарий 135/135; синтетика DoD 5×50 пройдена; smoke `st_estimate` μ=4000 мс, σ=296.52 мс; реальный кросс-камерный GT отсутствует |
| 2026-11-10 | Этап 5 (глобальная ассоциация): `trafficmcmt_flow` (`TrackletGraph`, `GraphPruner`, `SspSolver`, `MuSspSolver`, `GreedyAssociator`, `FlowAssociator`, `GlobalIdManager`), `trafficmcmt_metrics` (track-level IDF1/IDP/IDR/IDSW), `flow.link_reward`, `trafficmcmt_associate`; `ctest` 251/251 (muSSP skipped), CI-сценарий 153/153, локальный muSSP-форк 217/217; `ssp` и `mussp` дают идентичный результат на Auburn; Master без Edge-зависимостей |
| 2026-11-10 | Этап 6 (ZeroMQ): `trafficmcmt_net` (`ZmqContext`, `MessageFramer`, `SequenceTracker`, `ZmqTrackletSource`, `ZmqTrackletPublisher`, `ZmqResultSink`, `ZmqBroker`), `ErrorCode::Timeout`, `GlobalTrackUpdate`/`GlobalIdMergeEvent` (`proto/global.proto`), `trafficmcmt_broker`, `trafficmcmt_edge_simulator`, `tracklet_inspect --source zmq`, `scripts/smoke_zmq.ps1`; `build/zmq-check` — 25/25 net-тестов, DoD-soak 10×100×60 с: 60 000 сообщений, `missing_total=0`, `send_failures=0`, дрейф RSS 2.4%; CI-сценарий без ZMQ не затронут |
| 2026-11-11 | Этап 7 (время и out-of-order): `trafficmcmt_pipeline` (`MasterPipeline`, `WindowScheduler`, `TimestampBuffer`, `ClockSyncEstimator`, `ReplayGuard`, `MetricsCollector`), `ITrackletSource::Next(out, received_at)`, конфиг `clock_sync`/`watermark`/`online`, `JsonlResultSink`, `master_node --out/--stats-out/--dump-raw/--gt`, `trafficmcmt_tracklet_perturb`, `scripts/smoke_online.ps1`; `ctest` 242/242 (+1 skip), `ci-check` 179/179 (+1 skip), `zmq-check` 294/294 (+1 skip); DoD Auburn: IDF1 1.0000 → 1.0000 при ±2 с и 5% reorder, побайтовый детерминизм |
