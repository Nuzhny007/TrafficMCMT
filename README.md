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

Оба документа — **живые спецификации**: обновляются вместе с кодом.

## Статус

| Этап | Статус |
|---|---|
| 1. Инфраструктура (CMake, конфиг, логи, CLI, тесты) | 🟡 в работе (CI) |
| 2. Протокол и файловый I/O | ✅ готов |
| 2a. Тестовый пример на реальных видео (Aire, Auburn) | ✅ готов |
| 2b. Карта и viewer-фид (WebSocket v1) | ✅ готов |
| 3. FAISS и ReID | ⬜ |
| 4. st-ReID | ⬜ |
| 5. muSSP и глобальная ассоциация | ⬜ |
| 6. ZeroMQ транспорт | ⬜ |
| 7. Синхронизация времени | ⬜ |
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
Одиночная камера Aire: `configs/map_feed_aire.yaml`.

## Структура

```
include/trafficmcmt/core/   доменные типы, конфиг, Result, логирование, Geo (WGS84↔ENU)
include/trafficmcmt/io/     codec, file source/sinks, normalizer, store
include/trafficmcmt/edge/   ReID-экстракторы, EdgeTrackExporter, GeoBinding
include/trafficmcmt/mtt/    адаптер Multitarget-tracker → Observation
include/trafficmcmt/map/    MapFeed, MapProtocol, MapConfig, timeline, velocity, trail
include/trafficmcmt/net/    WebSocket-сервер/клиент (RFC 6455, без зависимостей)
proto/                      protobuf-схемы v1 (tracklet, calibration)
src/                        реализации core/io/edge/mtt/map/net
apps/master_node/           точка входа Master
apps/mtt_export/            видео → треклеты (Edge, OpenCV + MTT)
apps/map_export/            треклеты → JSONL-кадры протокола v1
apps/map_gateway/           WebSocket-реплей кадров для viewer
apps/tracklet_inspect/      инспектор треклетов
tests/unit/                 unit-тесты (GTest)
configs/                    примеры конфигурации (включая map_feed*.yaml)
cmake/                      зависимости, warnings, runtime-DLL, package config
docs/                       архитектура и roadmap
scripts/                    run_aire, run_auburn_toomer, smoke_map_gateway
```

Viewer (`traffic-mcmt-viewer`) — отдельный репозиторий: React + deck.gl + MapLibre,
подключается к `map_gateway` по `ws://localhost:8080/ws`.

## Лицензия

См. [LICENSE](LICENSE).
