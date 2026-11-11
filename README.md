# TrafficMCMT

Master-узел многокамерного трекинга (Multi-Camera Multi-Target, MCMT) для дорожного
трафика. Реализует **late fusion**: принимает треклеты от Edge-узлов
([Multitarget-tracker](https://github.com/Nuzhny007/Multitarget-tracker)), выполняет
межкамерный Re-identification, пространственно-временную фильтрацию и глобальную
ассоциацию через min-cost flow, выдавая стабильные глобальные ID.

## Документация

| Документ | Содержание |
|---|---|
| [`docs/architecture.md`](docs/architecture.md) | топология, потоки данных, протокол, алгоритмы, ADR |
| [`docs/roadmap.md`](docs/roadmap.md) | этапы, DoD, тесты, оценки, реестр рисков |
| [`docs/reid-eval.md`](docs/reid-eval.md) | протокол оценки ReID, схема manifest, формулы CMC/mAP |
| [`docs/emissions-attribution.md`](docs/emissions-attribution.md) | происхождение HBEFA-таблицы, лицензии |

Оба документа — **живые спецификации**: обновляются вместе с кодом.

## Статус

| Этап | Статус |
|---|---|
| 1. Инфраструктура (CMake, конфиг, логи, CLI, тесты) | 🟡 в работе (CI) |
| 2. Протокол и файловый I/O | ✅ готов |
| 2a. Тестовый пример на реальных видео (Aire, Auburn) | ✅ готов |
| 2b. Карта и viewer-фид (WebSocket v1) | ✅ готов |
| 2c. Gaussian puff: выбросы и концентрации | ✅ готов |
| 3. FAISS и ReID | 🟡 индекс/галереи/матчер и оценка готовы; FAISS не проверен |
| 4. st-ReID | ✅ Camera Link Model, переходы, гейты, scorer; реальный GT отсутствует |
| 5. muSSP и глобальная ассоциация | ✅ `ssp`/`greedy`/`mussp`, IDF1/IDSW, offline CLI; muSSP — опционально |
| 6. ZeroMQ транспорт | ✅ PUB/SUB, broker, result sink, edge simulator; smoke 60k msg без потерь |
| 7. Синхронизация времени и out-of-order | ✅ clock sync, watermark, окна/commit_lag, online-режим; DoD на Auburn |
| 8. BEV | ⬜ |

## Требования

- C++20, CMake ≥ 3.21
- OpenCV 4.12 (только для Edge-моста и BEV; Master собирается CPU-only)
- Protobuf 3.21+ с CMake-таргетами `protobuf::libprotobuf` и `protobuf::protoc`
- [Multitarget-tracker](https://github.com/Nuzhny007/Multitarget-tracker) установленный
  (`MTTracking_DIR` → `<install>/lib/MTTracking/cmake`), собранный **тем же компилятором**,
  что и TrafficMCMT, против той же версии OpenCV, с
  `USE_OCV_KCF=OFF`, `USE_OCV_UKF=OFF`, `USE_OCV_EMBEDDINGS=OFF`
- spdlog, yaml-cpp, GoogleTest (находятся через `find_package`, иначе — `FetchContent`)
- muSSP — **опционально** (`TRAFFICMCMT_WITH_MUSSP=ON`); путь к локальному checkout
  задаётся `TRAFFICMCMT_MUSSP_SOURCE_DIR` (должен содержать `muSSP/Graph.cpp`).
  Без muSSP работают `flow.solver=ssp|greedy`.
- Eigen 3 для ReID (`find_package`, иначе `FetchContent` 3.4.0)
- FAISS — опционально, `TRAFFICMCMT_WITH_FAISS=ON`; без него используется `BruteForceIndex`
- ZeroMQ — опционально, `TRAFFICMCMT_WITH_ZMQ=ON` (по умолчанию OFF, требует
  protobuf). Нужны `cppzmq`/`libzmq` (например, `vcpkg install cppzmq:x64-windows`).
  Без ZMQ `trafficmcmt_net` собирается только с WebSocket-транспортом.

Локальная сборка MTT (путь по умолчанию для `CMakeUserPresets.json`):

```powershell
git clone https://github.com/Nuzhny007/Multitarget-tracker.git C:\work\home\mtracker\TrafficMCMT\Multitarget-tracker
cmake -S C:\work\home\mtracker\TrafficMCMT\Multitarget-tracker `
      -B C:\work\home\mtracker\TrafficMCMT\Multitarget-tracker\build `
      -G "Visual Studio 18 2026" -A x64 `
      -DCMAKE_INSTALL_PREFIX=C:/work/home/mtracker/TrafficMCMT/Multitarget-tracker/install `
      -DOpenCV_DIR=C:/work/tools/vcpkg/installed/x64-windows/share/opencv4 `
      -DCMAKE_PREFIX_PATH=C:/work/tools/vcpkg/installed/x64-windows `
      -DBUILD_EXAMPLES=OFF -DUSE_OCV_KCF=OFF -DUSE_OCV_UKF=OFF -DUSE_OCV_EMBEDDINGS=OFF
cmake --build C:\work\home\mtracker\TrafficMCMT\Multitarget-tracker\build --config Release --parallel 4
cmake --install C:\work\home\mtracker\TrafficMCMT\Multitarget-tracker\build --config Release
```

## Сборка (Windows / MSVC)

```powershell
cmake --preset local-release
cmake --build --preset local-release
ctest --preset local-release
```

`CMakeUserPresets.json` содержит локальные пути к зависимостям этой машины и не
коммитится. На другой машине задайте пути вручную:

```powershell
cmake -S . -B build/release -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_PREFIX_PATH="<spdlog>;<yaml-cpp>;<gtest>;<opencv>" `
  -DMTTracking_DIR="<MTT install>/lib/MTTracking/cmake"
cmake --build build/release --config Release
ctest --test-dir build/release -C Release --output-on-failure
```

## Сборка (Linux)

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j
ctest --test-dir build/release --output-on-failure
```

## Запуск

```bash
trafficmcmt_master --config configs/master.yaml
trafficmcmt_master --config configs/master.yaml --dump-config
trafficmcmt_master --version
trafficmcmt_master --help
```

Экспорт треклетов из видео (Этап 2):

```bash
trafficmcmt_mtt_export --video data/atrium.avi --out data/tracklets.jsonl --speed 0
trafficmcmt_mtt_export --video data/atrium.avi --out data/tracklets.bin --quantize --speed 0
trafficmcmt_tracklet_inspect data/tracklets.jsonl
trafficmcmt_tracklet_inspect data/tracklets.bin --json --limit 5
```

Карта и viewer-фид (Этап 2b) — треклеты → протокол v1 → WebSocket:

```powershell
# offline: кадры протокола v1 в JSONL
.\build\local-release\Release\trafficmcmt_map_export.exe `
  --input build\datasets\auburn_toomer\combined.jsonl `
  --config configs\map_feed.yaml --out build\map\auburn_frames.jsonl

# WebSocket-реплей для traffic-mcmt-viewer (ws://localhost:8080/ws)
.\build\local-release\Release\trafficmcmt_map_gateway.exe `
  --input build\datasets\auburn_toomer\combined.jsonl `
  --config configs\map_feed.yaml --port 8080 --speed 1

# smoke с реальным WebSocket-клиентом
powershell -ExecutionPolicy Bypass -File scripts/smoke_map_gateway.ps1
```

`map_gateway` принимает `--speed` (0 — без pacing), `--loop`, `--exit-on-end`,
`--duration`, `--frames-out`; `/health` отдаёт JSON со счётчиком активных треков.
Раз в `feed.heartbeat_s` (5 с) шлётся кадр `heartbeat`, поэтому viewer отличает
тишину в данных от мёртвого соединения. Одиночная камера Aire:
`configs/map_feed_aire.yaml`.

Выбросы и концентрации (Этап 2c) считаются тем же `map_gateway`: при включённой
секции `emissions:` он раз в секунду шлёт кадр `emission_grid` (NOx, CO, PM2.5,
PM10, CO2 в мкг/м³). Модель — HBEFA + Gaussian puff, происхождение таблицы и
лицензии описаны в [`docs/emissions-attribution.md`](docs/emissions-attribution.md).

ReID: индекс, галереи и оценка (Этап 3):

```powershell
# chunk-split: чётные sequence -> галерея, нечётные -> запросы
.\build\local-release\Release\trafficmcmt_reid_eval.exe `
  --tracklets build\datasets\aire\cam-01.jsonl

# внешний размеченный сплит (JSONL, см. docs/reid-eval.md)
.\build\local-release\Release\trafficmcmt_reid_eval.exe `
  --manifest build\reid\manifest.jsonl --json

# латентность/QPS/recall индекса
.\build\local-release\Release\trafficmcmt_reid_bench.exe `
  --vectors 200000 --dim 512 --queries 200 --k 10 --index brute --json
```

Модуль `trafficmcmt_reid` — CPU-only (`core` + Eigen), не линкует OpenCV,
protobuf или MTT. `TRAFFICMCMT_WITH_FAISS=OFF` — рабочая конфигурация на
`BruteForceIndex`; запрос `faiss_*` в такой сборке возвращает `Unsupported`.
Протокол оценки, формулы и ограничения — в [`docs/reid-eval.md`](docs/reid-eval.md).

st-ReID: топология камер и оценка переходов (Этап 4):

```powershell
# размеченные пары: JSONL {"from": <tracklet_id>, "to": <tracklet_id>}
.\build\local-release\Release\trafficmcmt_st_estimate.exe `
  --tracklets build\datasets\auburn_toomer\combined.jsonl `
  --pairs build\st\pairs.jsonl `
  --out configs\camera_links.yaml `
  --min-samples 3
```

`trafficmcmt_st_estimate` группирует пары по направленному ребру `(from, to)`,
считает робастные `μ`/`σ` (median/MAD + trim) и пишет YAML-модель. Модель
подключается через `st.camera_links` в `configs/master.yaml`; шаблон Auburn —
`configs/camera_links.yaml`. Зоны входа/выхода задаются в ENU-метрах общего
origin. `default_linked: false` — без явных `links` пары не разрешены.

Модуль `trafficmcmt_st` — CPU-only (`core` + `reid`), собирается при
`EDGE=OFF, MTT=OFF, PROTOBUF=OFF`. Формулы `c_time`/`c_space`/`c_total` и
описание гейтов — в `docs/architecture.md` §8.1 и §8.5.

Глобальная ассоциация (Этап 5) — offline CLI `trafficmcmt_associate`:

```powershell
# треклеты + топология камер -> JSONL с глобальными ID
.\build\local-release\Release\trafficmcmt_associate.exe `
  --tracklets build\datasets\auburn_toomer\combined.jsonl `
  --camera-links configs\camera_links.yaml `
  --out build\associate\ids.jsonl `
  --solver ssp --json

# оценка IDF1/IDP/IDR/IDSW по GT
.\build\local-release\Release\trafficmcmt_associate.exe `
  --tracklets build\datasets\auburn_toomer\combined.jsonl `
  --gt build\associate\gt.jsonl `
  --out build\associate\ids.jsonl --json
```

`--solver` принимает `ssp` (по умолчанию, эталон), `greedy` (baseline) и
`mussp` (только при `TRAFFICMCMT_WITH_MUSSP=ON`). Переход выбирается при
`c_total < flow.link_reward` (по умолчанию 1.0). `--gt` — JSONL
`{"tracklet": <id>, "identity": "<gt-id>"}`. Модули `trafficmcmt_flow` и
`trafficmcmt_metrics` — CPU-only (`core`), собираются в CI-сценарии без Edge,
MTT и protobuf. Детали MCF и метрик — `docs/architecture.md` §8.2, §8.3, §8.6.

ZeroMQ транспорт (Этап 6) — приём потоков от N камер:

```powershell
# конфигурация с ZMQ (требует protobuf + cppzmq/libzmq)
cmake -S . -B build/zmq-check -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_PREFIX_PATH="C:/work/tools/vcpkg/installed/x64-windows" `
  -DTRAFFICMCMT_WITH_ZMQ=ON
cmake --build build/zmq-check --config Release --parallel 8

# broker (XSUB/XPUB): publishers -> :5555, subscribers -> :5556
.\build\zmq-check\Release\trafficmcmt_broker.exe

# N виртуальных камер
.\build\zmq-check\Release\trafficmcmt_edge_simulator.exe `
  --endpoint tcp://127.0.0.1:5555 --cameras 10 --rate 100 --duration 60

# consumer/инспектор поверх SUB
.\build\zmq-check\Release\trafficmcmt_tracklet_inspect.exe `
  --source zmq --endpoint tcp://127.0.0.1:5556 --duration 60

# end-to-end smoke: broker + 10 камер x 100 трекл./с x 60 с
powershell -ExecutionPolicy Bypass -File scripts/smoke_zmq.ps1 `
  -Duration 60 -Cameras 10 -Rate 100
```

Топики — `tmcmt.v1.<camera>.tracklet` и `tmcmt.v1.global.{track,events}`,
сообщение — multipart `[topic][payload]` с protobuf `TrackletChunk` /
`GlobalTrackUpdate` / `GlobalIdMergeEvent`. PUB-отправка неблокирующая:
переполнение очереди учитывается в `send_failures`, а не тормозит трекер.
`ZmqTrackletSource` считает `missing_total`/`duplicates`/`out_of_order` по
`sequence` на камеру. Схема и параметры — `docs/architecture.md` §7.8.

Синхронизация времени и онлайн-режим (Этап 7):

```powershell
# offline-эталон (псевдо-GT: identity = global_id)
.\build\local-release\Release\trafficmcmt_master.exe `
  --config configs\master.yaml --mode offline `
  --out build\online\offline.jsonl --stats-out build\online\offline.stats.json

# online по тому же файлу
.\build\local-release\Release\trafficmcmt_master.exe `
  --config configs\master.yaml --mode online `
  --out build\online\online.jsonl --stats-out build\online\online.stats.json

# DoD: ±2 с джиттера доставки, 5% переупорядочивания, сравнение IDF1
powershell -ExecutionPolicy Bypass -File scripts/smoke_online.ps1
```

`trafficmcmt_master` поддерживает `--out` (JSONL обновлений, merges — в
`<path>.merges.jsonl`), `--stats-out` (JSON-статистика и IDF1), `--dump-raw`
(raw/corrected времена и оценка смещения), `--gt` (JSONL предыдущего прогона
как псевдо-GT) и `--max-events`/`--stop-after-idle-ms`. Для файлового реплея
clock sync принудительно выключается: receive-time файла — не часы Edge. В
ZMQ-режиме sync работает по парам `(tracklet.end, время приёма Master)`.
`online.clock = event` делает реплей побайтово детерминированным.

`trafficmcmt_tracklet_perturb` создаёт out-of-order поток: `--jitter-ms`
сдвигает **порядок доставки** (метки времени не меняются), `--reorder` —
вероятность обмена соседних сообщений, `--event-jitter-ms` — отдельный
стресс-режим порчи меток времени. Детали — `docs/architecture.md` §8.7.

## Структура

```
include/trafficmcmt/core/   доменные типы, конфиг, Result, логирование, Geo (WGS84↔ENU)
include/trafficmcmt/io/     codec, file source/sinks, normalizer, store
include/trafficmcmt/edge/   ReID-экстракторы, EdgeTrackExporter, GeoBinding
include/trafficmcmt/mtt/    адаптер Multitarget-tracker → Observation
include/trafficmcmt/map/    MapFeed, MapProtocol, MapConfig, timeline, velocity, trail
include/trafficmcmt/net/    ZeroMQ-транспорт (context, framer, source, publisher, result sink, broker)
                            и WebSocket-сервер/клиент (RFC 6455, без зависимостей)
include/trafficmcmt/emissions/  HBEFA, Pasquill-Gifford, Gaussian puff, сетка концентраций
include/trafficmcmt/reid/   IEmbeddingIndex, BruteForceIndex, FaissIndex, GalleryManager, ReIdMatcher
include/trafficmcmt/st/     CameraLinkModel, TransitionEstimator, SpatialGate, StReIdScorer, PairCandidateGenerator
include/trafficmcmt/flow/   TrackletGraph, GraphPruner, McfSolver, FlowAssociator, GlobalIdManager
include/trafficmcmt/metrics/  MotMetrics (track-level IDF1/IDP/IDR/IDSW)
include/trafficmcmt/pipeline/ MasterPipeline, WindowScheduler, TimestampBuffer, ClockSyncEstimator, ReplayGuard, MetricsCollector
proto/                      protobuf-схемы v1 (tracklet, calibration, global)
src/                        реализации core/io/edge/mtt/map/net/emissions/reid/st/flow/metrics/pipeline
apps/master_node/           точка входа Master (offline/online, JSONL/ZMQ)
apps/mtt_export/            видео → треклеты (Edge, OpenCV + MTT)
apps/map_export/            треклеты → JSONL-кадры протокола v1
apps/map_gateway/           WebSocket-реплей кадров для viewer
apps/tracklet_inspect/      инспектор треклетов
apps/tracklet_perturb/      out-of-order поток для проверки DoD Этапа 7
apps/reid_eval/             CMC/Rank-1/mAP (chunk-split и manifest)
apps/reid_bench/            латентность, QPS, recall@k индекса
apps/st_estimate/           размеченные пары → статистика переходов → camera_links.yaml
apps/associate/             offline-ассоциация: треклеты → глобальные ID + IDF1/IDSW
apps/broker/                XSUB/XPUB-форвардер для ZeroMQ
apps/edge_simulator/        N виртуальных камер для нагрузочных прогонов
tests/unit/                 unit-тесты (GTest)
configs/                    примеры конфигурации (включая map_feed*.yaml, camera_links.yaml)
cmake/                      зависимости, warnings, runtime-DLL, package config
docs/                       архитектура, roadmap, оценка ReID
scripts/                    run_aire, run_auburn_toomer, smoke_map_gateway, smoke_zmq, smoke_online, reid_eval_*
```

Viewer (`traffic-mcmt-viewer`) — отдельный репозиторий: React + deck.gl + MapLibre,
подключается к `map_gateway` по `ws://localhost:8080/ws`.

## Лицензия

См. [LICENSE](LICENSE).
