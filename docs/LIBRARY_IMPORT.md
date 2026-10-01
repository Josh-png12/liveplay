# LivePlay+: biblioteca e importación inicial

Estado actual: Phase 1.5 extiende el núcleo de fase 1 con Smart Library UI, almacenamiento por usuario, mantenimiento explícito e interfaces de análisis. La importación YouTube y waveform independiente siguen pendientes. No añade algoritmos de análisis musical ni modifica `.liveplay`, `ProjectState`, cues o reproducción. La base existente se conserva y extiende; no hay SQLite.

## Responsabilidades y reutilización

| Archivo / área | Responsabilidad |
|---|---|
| `server/include/liveplay/library/track.hpp` | Track independiente del AudioItem del proyecto; contrato JSON |
| `server/include/liveplay/library/track_repository.hpp` | Interfaz sustituible y repositorio JSON |
| `server/src/library/track_repository.cpp` | Catálogo validado, deduplicación y commits antes de publicar en memoria |
| `server/{include/liveplay,src}/library/library_service.*` | Fachada de biblioteca; paginación y registro |
| `server/include/liveplay/util/atomic_json.hpp`, `server/src/library/atomic_json.cpp` | Lectura acotada y sustitución atómica del fichero JSON |
| `server/{include/liveplay,src}/import/import_service.*` | Worker, copia/hash/metadata, lotes persistentes, recuperación y cancelación |
| `server/src/net/control_server.cpp`, su cabecera y `server/src/main.cpp` | Composición del servicio, rutas y directorio local |
| `server/CMakeLists.txt` | Fuentes y suite CTest `library` |
| `server/tests/library_test.cpp` | Persistencia, concurrencia, fallo de commit, deduplicación, importación y recuperación |
| `server/tests/e2e/plus-library-e2e.js` | HTTP, uploads, permisos, reinicio y aislamiento de proyecto/cues |
| `docs-site/public/api/reference.json` | Siete endpoints y schemas PlusTrack/PlusImportBatch |

Phase 1.5 añade `library/data_directory.*`, `library/maintenance.*`, `analysis/analysis.*`, `server/tests/plus_foundation_test.cpp`, `server/tests/library_benchmark.cpp`, `server/tests/e2e/plus-library-ui.js`; en el cliente, `LibraryPanel.vue`, `usePlusLibrary.ts`, `types/plus.ts`, `utils/library.ts` y `tests/library-tests.js`. Integra la vista en `app.vue` y expone el wrapper REST autenticado para Plus. También corrige errores previos de type checking en cart/workspace/mixer/playlist/useProject/useAudioEngine. No cambia versiones npm ni el lockfile.

Se reutilizan `meta::read_metadata` (TagLib/miniaudio), conversiones de rutas Unicode, la política `path_within_fs_roots`, AuthGuard, Crow multipart, nlohmann-json y libsodium existentes. No se añaden dependencias.

Las rutas anteriores de upload/copy escriben medios del proyecto: esta entrega utiliza almacenamiento propio y endpoints nuevos. El worker de waveform anterior depende de IDs del proyecto; no se invoca para Tracks de biblioteca. Estas adaptaciones preservan la separación prevista en ARCHITECTURE. JSON es la alternativa inicial permitida detrás de TrackRepository; SQLite podrá sustituirla sin cambiar LibraryService ni ImportService.

## Modelo Track, schema 1

| Campo | Tipo / significado |
|---|---|
| `id` | String `track_<SHA-256 hexadecimal>`; independiente de IDs de cues |
| `title`, `artist`, `album` | Strings; título de tags o nombre original sin extensión; artista/álbum desconocidos vacíos |
| `duration_ms` | Entero positivo, milisegundos |
| `format` | Extensión/contenedor en minúsculas; no identifica el codec |
| `sample_rate`, `channels` | Enteros; cero si desconocidos |
| `asset_path` | Ruta absoluta UTF-8 de la copia propiedad de la biblioteca |
| `content_hash` | `sha256:<64 caracteres hex>` calculado sobre todos los bytes copiados |
| `size_bytes` | Tamaño positivo del asset |
| `import_origin` | `server_path` o `upload`; conserva la primera importación |
| `source_name`, `imported_at` | Nombre original y fecha UTC de incorporación |
| `status` | `available`, `missing` o `corrupt`; se conserva metadata aunque falte el asset |

Cambiar tags cambia el hash: la deduplicación es de archivos idénticos, no de grabaciones equivalentes. Un duplicado conserva los metadatos del primer Track. No se modifica el archivo original.

## Almacenamiento y trabajos

Ubicación por defecto Windows: `%LOCALAPPDATA%/LivePlay/plus/`. macOS: `~/Library/Application Support/LivePlay/plus/`; Linux: `$XDG_DATA_HOME/LivePlay/plus/` o `~/.local/share/LivePlay/plus/`. Ya no depende del directorio del ejecutable. Override mediante `plusDataDir` en `liveplay.json` o `LIVEPLAY_PLUS_DATA_DIR` (prioridad de la variable). Se resuelven variables Unicode mediante la API wide de Windows. Cambiar el directorio requiere reiniciar el servidor.

```text
library/catalog.json       # {schemaVersion: 1, tracks: [...]}
library/catalog.backup.json # commit anterior (retención acotada)
library/catalog.corrupt.*.json # copia del catálogo inválido para diagnóstico
library/staging/            # copia temporal mientras se calcula hash
library/uploads/            # uploads pendientes
library/imports/            # journals de lotes
assets/                     # medios propios
cache/                      # reservado para artefactos futuros
analysis/jobs.json          # jobs independientes
analysis/tracks/            # documentos analíticos opcionales
```

Un único worker procesa lotes y archivos secuencialmente. La copia usa bloques de 64 KiB; metadata y hashing se ejecutan fuera del hilo de audio. El registro de Track se confirma antes del resultado del trabajo. Si hay un reinicio entre ambos commits, la repetición encuentra el hash y devuelve `duplicate`. Los resultados terminados no se vuelven a procesar. Los uploads se eliminan después de guardar su resultado.

Estados del lote: `queued`, `running`, `completed`, `cancelled`; un fallo de escritura del journal detiene la ingestión y se informa como `failed` en memoria. `completed` significa que todos los archivos tienen resultado, aunque algunos hayan fallado. Estados por archivo: `queued`, `imported`, `duplicate`, `failed`, `cancelled`. Cada resultado contiene `source: {path, name}`, `status`, `track_id`, `error`.

La cancelación es cooperativa; no revierte Tracks confirmados. El cierre conserva pendientes para reanudarlos. TagLib no se interrumpe en mitad de una lectura de metadata. Si catálogo o journals son inválidos al iniciar, Plus se deshabilita y LivePlay continúa; el fichero inválido se conserva.

La migración inicial desde `<ejecutable>/liveplay-plus/library/` valida el catálogo, copia a un directorio temporal, remapea assets/uploads y publica el directorio nuevo mediante rename. No borra el origen ni sobrescribe un catálogo destino existente. Rechaza symlinks y destinos parciales; un fallo bloquea Plus con error explícito y conserva el show. Se debe inspeccionar/reparar un destino parcial antes de repetir la migración.

Los assets propios se guardan como rutas relativas UTF-8 con `/` dentro del catálogo, y se devuelven absolutos por API. Upload journals y jobs analíticos propios también usan referencias relativas cuando es posible; rutas de importación externas siguen siendo absolutas. Mover un directorio Plus completo mantiene esas referencias. Las rutas absolutas de catálogos legacy se aceptan por compatibilidad.

El scheduler y repositorio analítico independientes están implementados como foundation sin proveedores; ver [ANALYSIS_ARCHITECTURE](ANALYSIS_ARCHITECTURE.md). No se encolan análisis automáticamente durante imports.

## HTTP

Se conserva la autenticación existente de operador. Al crear usuarios se requiere sesión; antes del primer usuario rige el comportamiento inicial de LivePlay. Los API tokens están excluidos de biblioteca/importación por acceso al filesystem; capabilities sigue disponible para tokens autorizados. No hay mensajes WebSocket nuevos: se consulta el lote por HTTP.

| Método | Ruta | Resultado |
|---|---|---|
| GET | `/api/plus/capabilities` | Disponibilidad, schema, límites y objeto de capacidades `analysis` |
| GET | `/api/plus/library/tracks?offset=0&limit=100` | `{tracks, total, offset, limit}`; orden estable por ID |
| GET | `/api/plus/library/tracks/<id>` | Track o 404 |
| POST | `/api/plus/imports` | JSON `{files: ["ruta absoluta", ...], requestId?: "..."}`; 202 + lote |
| POST | `/api/plus/imports/upload` | Multipart con archivos y filename; 202 + lote |
| GET | `/api/plus/imports/<id>` | Lote y resultados por archivo o 404 |
| POST | `/api/plus/imports/<id>/cancel` | Solicitud de cancelación persistida o 404 |

Phase 1.5 amplía `/api/plus/capabilities` con el objeto `analysis` (siete capacidades en false), `analysisAvailable:false` y `storageError`. El antiguo booleano `analysis` es reemplazado por el objeto solicitado; consumidores Plus de fase 1 que asumían un booleano deben usar el agregado explícito. No afecta clientes ni endpoints LivePlay anteriores.

Re-vinculación: `POST /api/plus/imports` acepta `relinkTrackId` y exactamente una ruta, o `POST /api/plus/imports/upload?relinkTrackId=<id>` acepta un archivo. Conserva ID/metadata, exige SHA-256 idéntico y copia el archivo a assets propios. También permite reparar bytes corruptos. El resultado es un lote ordinario y la UI conserva la intención de relink al reintentar.

| Método | Ruta nueva | Comportamiento |
|---|---|---|
| GET / POST | `/api/plus/library/maintenance` | Progreso / iniciar `verify`, `rebuild` o `cleanup` |
| GET / POST | `/api/plus/library/recovery` | Inspeccionar backups / restaurar o resetear catálogo deshabilitado |
| GET / POST | `/api/plus/analysis/jobs` | Listar / encolar job de proveedor explícito |
| GET | `/api/plus/analysis/tracks/<id>` | Análisis opcional por Track; 404 si ausente |

`verify` comprueba archivos, SHA-256 y orphans; persiste missing/corrupt sin borrar metadata. `rebuild` hace lo anterior e incorpora audio no indexado usando tags disponibles; no puede reconstruir títulos originales sin tags desde un nombre hash. `cleanup` requiere `{operation:"cleanup",assets:["nombre"],confirm:true}` y elimina solo archivos huérfanos seleccionados, sin seguir symlinks ni eliminar referencias. Imports y mantenimiento comparten exclusión durante operaciones de assets; la reproducción no participa en ese bloqueo.

Recuperación de catálogo: al detectar uno inválido se conserva el fichero y se crea `catalog.corrupt.<timestamp>.json`; Plus queda deshabilitado. `POST recovery` con `{action:"restore",backup:"catalog.backup.json",confirm:true}` valida antes de reemplazar. Alternativamente `{action:"reset",confirm:true}` permite un catálogo vacío conservando todos los assets para reconstruir. Ambas acciones requieren reinicio y hacen backup del fichero anterior. No hay restauración ni limpieza destructiva automática.

Los snapshots de mantenimiento son de sesión, no jobs durables. Si se reinicia durante verificación/rebuild/cleanup, se conservan commits ya confirmados y se debe ejecutar explícitamente otra operación. Imports y análisis sí tienen recuperación durable propia.

`requestId` opcional (máximo 128 bytes) permite repetir la misma lista de rutas sin crear otro lote dentro del historial retenido; reutilizarlo con entradas distintas devuelve 400. Uploads no tienen requestId en esta entrega; sus bytes siguen deduplicándose.

Límites: 1–1000 archivos/lote; 64 lotes retenidos/activos; 10.000 Tracks; catálogo 64 MiB; journal 8 MiB; POST de rutas 1 MiB; página 1–500 Tracks. Al llenarse el historial se elimina un lote terminado, nunca uno activo; su requestId deja de estar reservado. Los límites son protección inicial, no una garantía de rendimiento para 10.000 Tracks.

El tamaño por asset usa `--max-upload-mb` existente (256 MiB por defecto). En multipart el límite cubre el cuerpo completo incluyendo cabeceras. Extensiones admitidas: wav, mp3, flac, ogg, m4a, aac, aif, aiff, opus, wma; además se exige metadata válida y duración positiva. La aceptación por extensión no garantiza que el motor actual decodifique todos sus codecs; no hay transcodificación.

Errores HTTP: 400 entrada inválida, 401/403 autenticación o ruta fuera de `--fs-root`, 404 ID desconocido, 413 cuerpo excesivo, 429 cola/journal demasiado grande, 500 error de almacenamiento, 503 Plus deshabilitado. Un fallo de archivo posterior al 202 aparece en el resultado del lote.

## Verificación

```sh
cmake --build server/build --config Release
ctest --test-dir server/build -C Release --output-on-failure
node server/tests/e2e/plus-library-e2e.js server/build/liveplay-server.exe
node docs-site/scripts/check-api-reference.mjs
```

El E2E copia ejecutable y DLLs a un directorio temporal bajo `server/build`, usa el puerto 4579 y genera WAVs locales. Comprueba SHA-256 con Node crypto, rutas Unicode, importación parcial, upload y rollback, cancelación de 1000 entradas, persistencia tras reiniciar, sesiones/API tokens, catálogo corrupto y que proyecto/cues anteriores no cambian. No requiere descargar audio.

La validación ampliada comprende Release Windows, CTest, type checking del cliente, generación del renderer, las suites cliente existentes y la suite Library nueva. El E2E `--ui` usa Edge headless instalado (o `LIVEPLAY_TEST_BROWSER`), el renderer generado y CDP/Node para comprobar UI/API sin añadir Playwright. Los datos, ejecutable y perfil del navegador son desechables bajo `server/build`; puertos 4578/4579/4581. El test comprueba también fallo analítico sin cambiar snapshots de proyecto/cues, recovery explícito y limpieza seleccionada.

Comandos adicionales:

```sh
npm ci --no-audit --no-fund
npm run typecheck:client
npm run test:client
npm run build:client
node server/tests/e2e/plus-library-e2e.js server/build/liveplay-server.exe --ui
# Desde server/build: benchmark real con 100/500/1000 fixtures
liveplay-library-benchmark.exe ../../docs/LIBRARY_BENCHMARK_RESULTS.json
```

Resultados de rendimiento y sus límites: [LIBRARY_BENCHMARK](LIBRARY_BENCHMARK.md).

Validación final Phase 1.5 (Windows, 2026-10-01):

| Comprobación | Resultado |
|---|---|
| Servidor Release, MSVC/Ninja | Build correcto |
| CTest | 10/10 suites; cero fallos |
| Cliente `typecheck:client` | Correcto |
| Cliente `test:client` | Window bounds, settings placement, mixer regression y library correctos |
| Cliente `build:client` | Renderer generado correctamente |
| E2E Plus `--ui` | Diez bloques API/Edge correctos, incluidos recovery/reinicio y aislamiento de cues |
| Auditoría de referencia API | 137 method/path documentados, 31 schemas; WS sin cambios |
| Benchmark 100/500/1.000 | Cero fallos; 1,298 / 11,314 / 35,603 segundos |

Los builds no incluyen empaquetado/signing del instalador Electron ni validación macOS/Linux. CTest valida DSP existente; el E2E comprueba aislamiento de datos/control, no continuidad acústica bajo carga.

El entorno carecía de CMake/Ninja/vcpkg y de un SDK de Windows completo. Se prepararon herramientas/SDK locales en `server/build-tools` (ignorado), sin cambios a presets, dependencias declaradas, updater o CI. Se requirió ejecutar build/E2E fuera del sandbox para permitir auxiliares y el proceso servidor local.

## Límites y siguiente trabajo

- Repositorio JSON mantiene el catálogo en memoria y reescribe su contenido por Track; el benchmark confirma un cuello de botella de persistencia para fixtures pequeños. Propuesta de siguiente optimización: batching, después journal incremental/compaction o SQLite con mediciones comparables. No se implementó cambio de storage.
- Un único proceso debe ser dueño del directorio. No hay bloqueo entre procesos ni eliminación/edición general de Tracks. La migración copia los datos legacy; se conserva un snapshot anterior del catálogo y la recuperación exige una operación explícita.
- Las rutas propias del catálogo nuevo son portables; referencias externas/legacy absolutas pueden requerir re-vinculación. Un duplicado normal con asset faltante sigue fallando y exige la operación explícita de relink.
- La CLI existente construye `--fs-root` desde argv estrecho sin conversión UTF-8 explícita en Windows; una raíz Unicode falló en el E2E. Se usa raíz CLI ASCII y nombres Unicode por HTTP, que sí pasan. Corregir la CLI queda fuera de esta entrega y no se relajó la política de rutas.
- Un cierre abrupto puede dejar staging/uploads/assets huérfanos. Existe detección/limpieza explícita de assets, sin recolección automática de staging/uploads. Atomic rename no equivale a una transacción conjunta de catálogo, asset y journal ni garantiza supervivencia ante cualquier corte eléctrico.
- Los datos compartidos son locales al servidor, sin permisos por usuario. Debe configurarse bind/autenticación/fs-root conforme al despliegue.
- Smart Library UI disponible desde el botón Biblioteca musical, con o sin proyecto. Incluye uploads en grupos acotados, polling cancelable, búsqueda/orden/filtros, paginación, duplicados, errores por archivo, cancelación/reintento, relink y mantenimiento/recovery explícitos. Los File del navegador no persisten al cerrar; para reintentar uploads tras reabrir se deben seleccionar otra vez. Los mensajes nuevos están en español y aún requieren integración completa en todos los locales existentes.
- No hay YouTube, waveform de biblioteca, transcodificación ni análisis musical. Ninguna llamada de Internet/LLM participa en esta ruta. No se midió todavía continuidad acústica bajo contención ni empaquetado/instalación limpia.
