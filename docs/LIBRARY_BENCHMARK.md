# LivePlay+ — benchmark de importación JSON

Medición local del 1 de octubre de 2026, Release Windows, MSVC 19.51 / SDK 10.0.26100. Se conservan TrackRepository e ImportService existentes; no se añadió SQLite ni otra dependencia. Resultado bruto: [LIBRARY_BENCHMARK_RESULTS.json](LIBRARY_BENCHMARK_RESULTS.json).

## Método

`server/tests/library_benchmark.cpp` genera 1.000 WAV distintos de un segundo, PCM estéreo 48 kHz / 16 bits, 192.044 bytes cada uno. Importa 100, 500 y 1.000 archivos mediante el ImportService real, en tres catálogos vacíos separados. Usa el layout de producción `library/catalog.json` + `assets/`, referencias relativas, snapshots y journals actuales. Los fixtures se crean y eliminan en un directorio único bajo el cwd del benchmark; ejecutarlo desde `server/build`.

Se registra una corrida por tamaño, sin build ni E2E concurrentes. Hubo una corrida de calentamiento; no se vació la caché del sistema. Estos tiempos no son percentiles ni una garantía para otros discos, antivirus, codecs, canciones completas o catálogos preexistentes. No incluyen generación de fixtures ni upload HTTP. Tampoco miden continuidad acústica.

- Hashing: actualizaciones SHA-256 durante la copia; inicialización/finalización quedan en otros costes.
- Copia: lectura de origen y escritura de staging; cierre/publicación y comprobaciones quedan en otros costes.
- Metadata: extractor TagLib/miniaudio existente.
- Persistencia de catálogo: registro del Track, snapshot anterior y reemplazo JSON.
- Persistencia de journal: tiempo acumulado de escrituras durables del lote.
- Total: submit hasta estado terminal observado; incluye serialización, sincronización, validación, publicación y polling. `other_ms` es el residual, no una medición de CPU.

## Resultados

Tiempos acumulados en segundos. Persistencia = catálogo + journal.

| Tracks | SHA-256 | Metadata | Copia | Catálogo | Journal | Persistencia | Total | Catálogo final |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 100 | 0,080 | 0,024 | 0,049 | 0,493 | 0,435 | 0,927 | 1,298 | 63.622 bytes |
| 500 | 0,462 | 0,126 | 0,290 | 4,972 | 3,372 | 8,345 | 11,314 | 318.822 bytes |
| 1.000 | 0,955 | 0,258 | 0,626 | 16,056 | 10,832 | 26,887 | 35,603 | 637.822 bytes |

Cero archivos fallidos en los tres tamaños. El catálogo crece aproximadamente de forma lineal, pero persistencia consume entre 71 % y 76 % del tiempo. Multiplicar entradas por diez multiplica el total por aproximadamente 27: las reescrituras completas de catálogo y lote son un cuello de botella en estos fixtures pequeños. No extrapolar este resultado a 10.000 Tracks como garantía.

## Propuesta de optimización, sin implementar migración

1. Batching de commits/journal en el worker, con tamaño/intervalo acotado. Definir primero cuándo un Track se considera durable, qué muestra la UI y cómo recuperar una interrupción. Un debounce que anuncie éxito antes de persistir perdería garantías actuales.
2. Journal incremental con replay idempotente y compaction. Medir write amplification, tiempo de arranque, crecimiento, fallos de escritura y retención; mantener snapshots recuperables.
3. Evaluar SQLite detrás de TrackRepository cuando se necesiten consultas/indexación o si las opciones anteriores no alcanzan objetivos medidos. Revisar empaquetado Windows, licencia, mantenimiento y transacciones catálogo/assets antes de adoptarlo.

Repetir esta misma herramienta tras cualquier optimización y añadir varias corridas, canciones largas/formatos comprimidos, catálogo preexistente y prueba de contención con audio activo. Esta entrega solo añade instrumentación y evidencia.

## Ejecución

Configurar el servidor con `-DLIVEPLAY_BUILD_TESTS=ON` y compilar Release siguiendo [DEVELOPMENT](DEVELOPMENT.md). El target es `liveplay-library-benchmark`.

```powershell
Set-Location server/build
./liveplay-library-benchmark.exe ../../docs/LIBRARY_BENCHMARK_RESULTS.json
```

Las métricas por archivo también aparecen en `files[].timings` del batch; `journal_ms` registra sus escrituras. El contador guardado en disco excluye la escritura que lo contiene, por lo que la herramienta usa el batch final en memoria.
