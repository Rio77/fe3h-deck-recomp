# FE3H Deck Recomp

[English](README.md) · [Instalación y recuperación](docs/linux.md) · [Validación](docs/validation.md)

Adaptación nativa para Linux de *Fire Emblem: Three Houses*, optimizada y probada en **Steam Deck LCD**. Se basa en **FE3H Recomp de onerag90**, construido sobre **LASRecomp de Peiikai**. Es un fork mantenido de forma independiente, no una publicación oficial de esos proyectos.

> [!IMPORTANT]
> El proyecto y la adaptación Linux se desarrollaron con asistencia de IA y dirección, pruebas y revisión humanas. Se comparten tal como están; las pruebas no cubren una partida completa ni todos los dispositivos.

> [!NOTE]
> No se incluyen juego, DLC, código recompilado del juego, shaders extraídos, fuentes de consola ni partidas. Necesitás tu propia copia obtenida legalmente de **FE3H 1.2.0**, con la actualización integrada en las carpetas extraídas `exefs` y `romfs`. Los DLC extraídos son opcionales. No se proporcionan instrucciones para obtener archivos del juego.

## Qué es

El recompilador estático traduce el código AArch64 original a C y lo compila localmente. El runtime nativo implementa los servicios del sistema que utiliza el juego, con gráficos Vulkan y bibliotecas nativas de audio y vídeo. Para jugar no se necesitan Wine ni Proton. FE3H Recomp y LASRecomp aportan la base; este fork adapta selectivamente su código y añade las optimizaciones Linux documentadas en [UPSTREAM.md](UPSTREAM.md).

## Estado

Versión actual: **0.3.1** (motor opt5 validado + Vulkan asíncrono / GPL fast-link + shaders de iluminación estructurados ampliados + soporte Amiibo y audio PulseAudio en modo escritorio). Probada en Steam Deck LCD en modo dock y portátil, conservando resoluciones internas ajustables, audio y cinemáticas. El destino publicado es Linux x86-64; otros dispositivos no recibieron la misma validación. Este repositorio no distribuye ni mantiene una compilación Windows.

Las mejoras de la 0.3.1 añaden **soporte nativo de Amiibo (`nn::nfp`)** con selector en *Ajustes > Game > Amiibo* (F1 / Minus+Plus) que escanea volcados `.bin` en `<carpeta de instalación>/Amiibo/` (volviendo automáticamente a `None` tras cada lectura completada en el Pabellón del Monasterio de Garreg Mach, esperando de forma segura si `None` está elegido y excluyendo cualquier archivo `.bin` de los paquetes de lanzamiento), y amplían la **selección automática de PulseAudio** (`open_audio_stream()` en `audio.c`) también al **Modo Escritorio** en Linux (corrigiendo la distorsión robótica de SDL3 con PipeWire en el Monasterio, respetando `SDL_AUDIO_DRIVER`, `FE3H_AUDIO_AUTO_PULSE=0` y el fallback automático a SDL).

Las mejoras de la 0.3.0 aportan un gran salto de rendimiento GPU en Steam Deck: amplían el flujo de control estructurado (`FE3H_LIGHTING_SCF`) a los fragment shaders de iluminación diferida y postprocesado más pesados de FE3H (`4b68a2b8cf21d44d`, `e205b94894daff2f` y `6f61034ed0d2c1dd`), reduciendo el tiempo GPU a 1080p nativo en el Monasterio de **30.46 ms (35.1 fps) a 18.58 ms (49.4 fps)** — una mejora de **~41% en FPS** (solo `4b68a2b8cf21d44d` pasa de 12.85 ms a 2.31 ms, 5.58× más rápido, con equivalencia de píxeles verificada) — y ejecutan `post_process()` (SMAA / AMD FSR 1) antes de abrir el render pass del swapchain para eliminar ciclos redundantes de `vk_frame_suspend`/`vk_frame_resume`.

Las mejoras de la 0.2.9 portan cambios seleccionados de upstream: **Integer Scaling** en *Ajustes > Video > Upscaler* (escalado entero nítido con bordes centrados para modo dock 2×/3×, con fallback suave en resolución dinámica o downsampling), **auto-selección de PulseAudio con fallback automático** en Linux Steam Game Mode (`open_audio_stream()` en `audio.c`) y **correcciones en la detección de archivos del juego** (priorizando `exefs/` + `romfs/` juntas, ignorando carpetas `mods/` y `dlc/` y limitando la detección de copia propia a `exefs`/`romfs`/`dlc`).

Las mejoras de la 0.2.8 añaden selector 30/60 FPS en ajustes (acoplado a la resolución dinámica FSR), enlazado rápido Vulkan `VK_EXT_graphics_pipeline_library` y estado dinámico de vértices, pre-filtrado y deduplicación por contenido de shaders compute en segundo plano, envío Vulkan asíncrono con copias seguras de listas NVN, caché persistente de UBOs (`CSNAP`, buffer cero y caché de descriptors), dispatch O(1) en 2 niveles, reparto multi-core Horizon, lecturas concurrentes sin bloqueo (`pread`) en RomFS, aislamiento de `mesa_cache` (sin Fossilize) y uso completo de los núcleos de la Steam Deck.

Opt5 reduce las esperas activas de CPU, añade precalentamiento de pipelines según el perfil y corrige la propiedad de recursos durante reinicios internos. También integra correcciones seleccionadas de GPU, texturas, mando, audio, cinemáticas, guardados y mods. La iluminación estructurada, precisión half nativa, memoria local dispersa y modo dock ya estaban en opt4.

En `match`, las comparaciones del monasterio mejoraron el p99 del tiempo de fotograma un **2,95 %** y el consumo de APU un **3,51 %** respecto de opt4. En batalla diurna, los FPS medios mejoraron un **2,15 %**, con consumo prácticamente igual y un **p99 un 1,71 % peor**. Un par tuvo diferencias térmicas que limitan su comparación. Son resultados de escenas concretas, no una garantía de 60 FPS constantes ni a x1/1080p. [Resultados y límites](docs/validation.md).

## Requisitos

- SteamOS / Linux x86-64; AppImage compilada para glibc 2.35 o posterior.
- CPU con AVX2/FMA y GPU/controlador con Vulkan 1.3. El hardware probado es la Deck LCD.
- Python 3.10+ y `kdialog` para instalar; SteamOS ya los incluye.
- Espacio escribible para el juego, una preparación separada y archivos temporales de compilación. Al reinstalar se conserva también la copia extraída anterior para recuperación.
- `exefs` y `romfs` propios de FE3H 1.2.0 con la actualización integrada; DLC extraídos opcionales.

## Instalar una publicación en SteamOS

La release ofrece un paquete de instalación para Steam Deck que ya contiene la AppImage. Descargá ese instalador para jugar. El código del proyecto en ZIP/TAR.GZ y las fuentes correspondientes completas en TAR.GZ están disponibles por separado para compilar y cumplir las licencias; no son pasos adicionales de instalación.

1. Descargá `FE3H-Deck-Recomp-0.3.1-steamdeck.tar.gz` desde [Releases](https://github.com/Rio77/fe3h-deck-recomp/releases/latest). Extraelo en una carpeta escribible desde el modo escritorio.
2. Conservá todos sus archivos juntos y abrí **Install.desktop**. Permití la ejecución si KDE lo solicita. Seleccioná tus `exefs`, `romfs` y, opcionalmente, la carpeta de DLC.
3. Esperá a que termine la copia y compilación local. La importación completa probada en la Deck tardó unos 43 minutos; puede variar según hardware, archivos y caché. No es el tiempo habitual de arranque del juego.
4. Abrí **FE3H Deck Recomp** desde el escritorio. Para usar el modo Juego, agregá manualmente el **FE3H.sh** instalado a Steam como juego externo.

El instalador gráfico recomienda `~/Games/FE3H-Deck-Recomp` y permite otro destino. En consola, sin `--root`, se mantiene `~/.local/share/fe3hrecomp`; los guardados están en `save`. Las instalaciones nuevas en Deck usan modo dock y `match`. Abrí ajustes con **F1** o **Back + Start**. El instalador no modifica SteamOS ni añade juegos a Steam sin intervención.

**Actualización y recuperación:** utilizá el instalador completo, no el actualizador automático de upstream ni una sobrescritura directa. Prepara y verifica una instalación separada, conserva partidas/configuración y retiene los datos extraídos anteriores. Consultá [instalación y recuperación](docs/linux.md) para las opciones de consola y `status` / `recover`.

### Si falla Install.desktop

Cerrá el instalador y abrí directamente la AppImage incluida desde el modo escritorio. Si hace falta, prepará tus archivos FE3H 1.2.0 desde **Settings > Game Files**, esperá a que termine la compilación y elegí **Play**. Esto no evita la validación de los archivos. Si la compilación ya terminó, conservá su carpeta de trabajo y registros para poder recuperarla.

Con esta alternativa, **conservá la AppImage** en una carpeta permanente y agregala a Steam manualmente. No se copia automáticamente mediante el instalador ni se crea su acceso administrado. Conservá también la carpeta de datos preparados.

## DLC y mods

Los DLC se suministran como carpetas del tipo `dlc/010055D009F79001/romfs/...`; se admiten los seis paquetes cuando se proporcionan y validan. Son opcionales. Los mods son de datos y se alojan en `extracted/mods`, separados del romfs original. Importá las carpetas desde el escritorio y activalas, desactivalas u ordenalas desde la interfaz. El último mod habilitado tiene prioridad; el orden se aplica en el siguiente arranque. Una instalación nueva no activa mods. El instalador no admite mods de código/exefs. Una partida con cambios de jugabilidad puede necesitar los mismos mods en otra instalación.

## Compilación y pruebas

Usá `make appimage ARCH=x86_64` con podman y conexión para descargar dependencias. La aplicación y las pruebas sintéticas de CI no necesitan el juego; su código se genera únicamente desde los archivos del usuario. Consultá [compilación](docs/linux.md), las recetas de dependencias y `.github/workflows/linux.yml`. La [primera ejecución pública de CI](https://github.com/Rio77/fe3h-deck-recomp/actions/runs/43784595531) superó la compilación y las pruebas sintéticas.

## Problemas conocidos y límites

- Existe una estela/desenfoque al comenzar combates en `match`, también presente en opt4. No se apreció en la comprobación manual a x1; aumentar la resolución incrementa considerablemente el coste de GPU.
- Los shaders de efectos enormes continúan excluidos en Deck; algunos efectos pueden perder una capa. La compilación inicial de shaders puede causar tirones o efectos incompletos temporalmente.
- Se probaron reinicios internos, una sesión continuada separada de 30 minutos y, previamente, mando y suspensión físicos. Esto no demuestra ausencia de pérdidas en todas las escenas ni durante varias horas.
- La comparación completa de framebuffer opt4/opt5 con entradas idénticas continúa pendiente. Se autorizó expresamente la promoción dejando documentado este límite.
- La versión pública guarda localmente. No contiene sincronización OneDrive, configuración de nube, claves de acceso ni credenciales.

Al informar un error, indicá versión, SteamOS/controlador, resolución, modo dock, pasos para reproducirlo y un registro revisado para quitar información privada. No publiques archivos del juego, bibliotecas generadas, shaders extraídos, partidas con datos personales ni credenciales.

## Créditos

- **[FE3H Recomp](https://git.onerag90.de/recomps/fe3h-recomp), de onerag90 y colaboradores:** adaptación original de Three Houses, servicios específicos, audio, cinemáticas, shaders y herramientas.
- **[LASRecomp](https://github.com/peiikai/LASRecomp), de Peiikai y colaboradores:** recompilador estático, runtime, renderizador y herramientas fundamentales. Su README conserva los créditos completos de upstream.
- **Colaboradores de Ryujinx:** reverberación y procesamiento auxiliar de audio, bajo MIT; aviso conservado en `third_party/ryujinx/LICENSE.txt`.
- **Colaboradores de Dear ImGui, SDL, FFmpeg, shaderc/glslang/SPIRV-Tools, Vulkan Memory Allocator y SMAA:** interfaz, multimedia y gráficos.
- **[AMD FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR):** funciones auxiliares de shaders heredadas bajo MIT; no se activaron nuevas funciones FSR en opt5.
- **Colaboradores de Capstone, Unicorn, python-lz4, PyInstaller, Python y Zig:** generación, compilación y empaquetado locales.
- **Autores de Baloo 2 y EB Garamond**, incluido Georg Duffner: fuentes abiertas distribuidas.
- **Switchbrew y comunidad homebrew de Switch:** documentación del hardware y del sistema.

Las versiones, licencias y avisos se detallan en [THIRD_PARTY.md](THIRD_PARTY.md) y dentro de la AppImage. Los créditos corresponden a componentes retenidos: este fork no distribuye el randomizador de LASRecomp ni la integración de FSR excluida.

## Información legal y licencia

Proyecto de aficionados sin afiliación ni respaldo de Nintendo, Intelligent Systems o Koei Tecmo. *Fire Emblem*, sus personajes y emblemas pertenecen a sus respectivos titulares.

El código conserva la **[licencia MIT](LICENSE)** original y los avisos de copyright de Peiikai y onerag90. Las dependencias mantienen sus licencias. El instalador incluye los avisos en `licenses/`. El archivo completo de fuentes correspondientes, incluidos FFmpeg y Unicorn, se descarga por separado como TAR.GZ desde la misma release. Distribuí esas fuentes y avisos junto a los binarios. Consultá [THIRD_PARTY.md](THIRD_PARTY.md).

El mantenedor proporcionó el icono para esta distribución. La licencia MIT del código no cubre automáticamente la imagen ni concede derechos sobre sus personajes o emblemas. El fondo opcional del lanzador se genera localmente desde los archivos del usuario.

El instalador permite elegir el destino y recomienda `~/Games/FE3H-Deck-Recomp`. No mueve instalaciones existentes. Para actualizar, seleccioná su misma carpeta. La opción de consola `--root` conserva su significado.


### 0.2.4: acceso Settings

Abrí **Settings.desktop** en la carpeta instalada o **Settings** en el escritorio para gestionar los mods con la ruta de datos correcta. Cerrá primero el juego. Los cambios se aplican al siguiente arranque. Esta versión actualiza el empaquetado y los accesos de Linux; conserva el motor opt5 validado y no incorpora el motor oficial 0.2.4.


### 0.2.5: validación y escritura

Se incorporan advertencias SHA-256 para cuatro archivos pequeños del juego base, nombres Unicode con límites por caracteres, borrado UTF-8 seguro y Ctrl+A/C/X/V/Z. Las discrepancias de contenido permiten continuar; los errores de estructura anteriores siguen siendo errores. La comprobación se realiza antes de superponer mods y no verifica todos los datos del juego ni los DLC.

La visualización de cada alfabeto depende de las fuentes instaladas; las fuentes coreanas solo se cargan cuando hacen falta. El teclado de mando conserva su distribución latina. Esta versión mantiene opt5 con estas correcciones selectivas: no incorpora actualizador, redimensionado del menú, oscurecimiento ni reescaladores temporales.
