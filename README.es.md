# FE3H Deck Recomp

[English](README.md) Â· [InstalaciÃ³n y recuperaciÃ³n](docs/linux.md) Â· [ValidaciÃ³n](docs/validation.md)

AdaptaciÃ³n nativa para Linux de *Fire Emblem: Three Houses*, optimizada y probada en **Steam Deck LCD**. Se basa en **FE3H Recomp de onerag90**, construido sobre **LASRecomp de Peiikai**. Es un fork mantenido de forma independiente, no una publicaciÃ³n oficial de esos proyectos.

> [!IMPORTANT]
> El proyecto y la adaptaciÃ³n Linux se desarrollaron con asistencia de IA y direcciÃ³n, pruebas y revisiÃ³n humanas. Se comparten tal como estÃ¡n; las pruebas no cubren una partida completa ni todos los dispositivos.

> [!NOTE]
> No se incluyen juego, DLC, cÃ³digo recompilado del juego, shaders extraÃ­dos, fuentes de consola ni partidas. NecesitÃ¡s tu propia copia obtenida legalmente de **FE3H 1.2.0**, con la actualizaciÃ³n integrada en las carpetas extraÃ­das `exefs` y `romfs`. Los DLC extraÃ­dos son opcionales. No se proporcionan instrucciones para obtener archivos del juego.

## QuÃ© es

El recompilador estÃ¡tico traduce el cÃ³digo AArch64 original a C y lo compila localmente. El runtime nativo implementa los servicios del sistema que utiliza el juego, con grÃ¡ficos Vulkan y bibliotecas nativas de audio y vÃ­deo. Para jugar no se necesitan Wine ni Proton. FE3H Recomp y LASRecomp aportan la base; este fork adapta selectivamente su cÃ³digo y aÃ±ade las optimizaciones Linux documentadas en [UPSTREAM.md](UPSTREAM.md).

## Estado

VersiÃ³n actual: **0.2.3-linux-opt5**. Probada en Steam Deck LCD en modo dock, conservando resoluciones internas ajustables, audio y cinemÃ¡ticas. El destino publicado es Linux x86-64; otros dispositivos no recibieron la misma validaciÃ³n. Este repositorio no distribuye ni mantiene una compilaciÃ³n Windows.

Opt5 reduce las esperas activas de CPU, aÃ±ade precalentamiento de pipelines segÃºn el perfil y corrige la propiedad de recursos durante reinicios internos. TambiÃ©n integra correcciones seleccionadas de GPU, texturas, mando, audio, cinemÃ¡ticas, guardados y mods. La iluminaciÃ³n estructurada, precisiÃ³n half nativa, memoria local dispersa y modo dock ya estaban en opt4.

En `match`, las comparaciones del monasterio mejoraron el p99 del tiempo de fotograma un **2,95 %** y el consumo de APU un **3,51 %** respecto de opt4. En batalla diurna, los FPS medios mejoraron un **2,15 %**, con consumo prÃ¡cticamente igual y un **p99 un 1,71 % peor**. Un par tuvo diferencias tÃ©rmicas que limitan su comparaciÃ³n. Son resultados de escenas concretas, no una garantÃ­a de 60 FPS constantes ni a x1/1080p. [Resultados y lÃ­mites](docs/validation.md).

## Requisitos

- SteamOS / Linux x86-64; AppImage compilada para glibc 2.35 o posterior.
- CPU con AVX2/FMA y GPU/controlador con Vulkan 1.3. El hardware probado es la Deck LCD.
- Python 3.10+ y `kdialog` para instalar; SteamOS ya los incluye.
- Espacio escribible para el juego, una preparaciÃ³n separada y archivos temporales de compilaciÃ³n. Al reinstalar se conserva tambiÃ©n la copia extraÃ­da anterior para recuperaciÃ³n.
- `exefs` y `romfs` propios de FE3H 1.2.0 con la actualizaciÃ³n integrada; DLC extraÃ­dos opcionales.

## Instalar una publicaciÃ³n en SteamOS

1. ObtenÃ© el paquete completo `FE3H-Deck-Recomp-opt5-installer.tar.gz` desde Releases de este proyecto cuando se publique. Extraelo en una carpeta escribible desde el modo escritorio.
2. ConservÃ¡ todos sus archivos juntos y abrÃ­ **Install.desktop**. PermitÃ­ la ejecuciÃ³n si KDE lo solicita. SeleccionÃ¡ tus `exefs`, `romfs` y, opcionalmente, la carpeta de DLC.
3. EsperÃ¡ a que termine la copia y compilaciÃ³n local. La importaciÃ³n completa probada en la Deck tardÃ³ unos 43 minutos; puede variar segÃºn hardware, archivos y cachÃ©. No es el tiempo habitual de arranque del juego.
4. AbrÃ­ **FE3H Deck Recomp** desde el escritorio. Para usar el modo Juego, agregÃ¡ manualmente el **FE3H.sh** instalado a Steam como juego externo.

La carpeta de datos predeterminada continÃºa siendo `~/.local/share/fe3hrecomp`; los guardados estÃ¡n en `save`. Las instalaciones nuevas en Deck usan modo dock y `match`. AbrÃ­ ajustes con **F1** o **Back + Start**. El instalador no modifica SteamOS ni aÃ±ade juegos a Steam sin intervenciÃ³n.

**ActualizaciÃ³n y recuperaciÃ³n:** utilizÃ¡ el instalador completo, no el actualizador automÃ¡tico de upstream ni una sobrescritura directa. Prepara y verifica una instalaciÃ³n separada, conserva partidas/configuraciÃ³n y retiene los datos extraÃ­dos anteriores. ConsultÃ¡ [instalaciÃ³n y recuperaciÃ³n](docs/linux.md) para las opciones de consola y `status` / `recover`.

## DLC y mods

Los DLC se suministran como carpetas del tipo `dlc/010055D009F79001/romfs/...`; se admiten los seis paquetes cuando se proporcionan y validan. Son opcionales. Los mods son de datos y se alojan en `extracted/mods`, separados del romfs original. ImportÃ¡ las carpetas desde el escritorio y activalas, desactivalas u ordenalas desde la interfaz. El Ãºltimo mod habilitado tiene prioridad; el orden se aplica en el siguiente arranque. Una instalaciÃ³n nueva no activa mods. El instalador no admite mods de cÃ³digo/exefs. Una partida con cambios de jugabilidad puede necesitar los mismos mods en otra instalaciÃ³n.

## CompilaciÃ³n y pruebas

UsÃ¡ `make appimage ARCH=x86_64` con podman y conexiÃ³n para descargar dependencias. La aplicaciÃ³n y las pruebas sintÃ©ticas de CI no necesitan el juego; su cÃ³digo se genera Ãºnicamente desde los archivos del usuario. ConsultÃ¡ [compilaciÃ³n](docs/linux.md), las recetas de dependencias y `.github/workflows/linux.yml`. CI estÃ¡ preparada, pero todavÃ­a no se ejecutÃ³ en un repositorio remoto de GitHub.

## Problemas conocidos y lÃ­mites

- Existe una estela/desenfoque al comenzar combates en `match`, tambiÃ©n presente en opt4. No se apreciÃ³ en la comprobaciÃ³n manual a x1; aumentar la resoluciÃ³n incrementa considerablemente el coste de GPU.
- Los shaders de efectos enormes continÃºan excluidos en Deck; algunos efectos pueden perder una capa. La compilaciÃ³n inicial de shaders puede causar tirones o efectos incompletos temporalmente.
- Se probaron reinicios internos, una sesiÃ³n continuada separada de 30 minutos y, previamente, mando y suspensiÃ³n fÃ­sicos. Esto no demuestra ausencia de pÃ©rdidas en todas las escenas ni durante varias horas.
- La comparaciÃ³n completa de framebuffer opt4/opt5 con entradas idÃ©nticas continÃºa pendiente. Se autorizÃ³ expresamente la promociÃ³n dejando documentado este lÃ­mite.
- La versiÃ³n pÃºblica guarda localmente. No contiene sincronizaciÃ³n OneDrive, configuraciÃ³n de nube, claves de acceso ni credenciales.

Al informar un error, indicÃ¡ versiÃ³n, SteamOS/controlador, resoluciÃ³n, modo dock, pasos para reproducirlo y un registro revisado para quitar informaciÃ³n privada. No publiques archivos del juego, bibliotecas generadas, shaders extraÃ­dos, partidas con datos personales ni credenciales.

## CrÃ©ditos

- **[FE3H Recomp](https://git.onerag90.de/recomps/fe3h-recomp), de onerag90 y colaboradores:** adaptaciÃ³n original de Three Houses, servicios especÃ­ficos, audio, cinemÃ¡ticas, shaders y herramientas.
- **[LASRecomp](https://github.com/peiikai/LASRecomp), de Peiikai y colaboradores:** recompilador estÃ¡tico, runtime, renderizador y herramientas fundamentales. Su README conserva los crÃ©ditos completos de upstream.
- **Colaboradores de Ryujinx:** reverberaciÃ³n y procesamiento auxiliar de audio, bajo MIT; aviso conservado en `third_party/ryujinx/LICENSE.txt`.
- **Colaboradores de Dear ImGui, SDL, FFmpeg, shaderc/glslang/SPIRV-Tools, Vulkan Memory Allocator y SMAA:** interfaz, multimedia y grÃ¡ficos.
- **[AMD FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR):** funciones auxiliares de shaders heredadas bajo MIT; no se activaron nuevas funciones FSR en opt5.
- **Colaboradores de Capstone, Unicorn, python-lz4, PyInstaller, Python y Zig:** generaciÃ³n, compilaciÃ³n y empaquetado locales.
- **Autores de Baloo 2 y EB Garamond**, incluido Georg Duffner: fuentes abiertas distribuidas.
- **Switchbrew y comunidad homebrew de Switch:** documentaciÃ³n del hardware y del sistema.

Las versiones, licencias y avisos se detallan en [THIRD_PARTY.md](THIRD_PARTY.md) y dentro de la AppImage. Los crÃ©ditos corresponden a componentes retenidos: este fork no distribuye el randomizador de LASRecomp ni la integraciÃ³n de FSR excluida.

## InformaciÃ³n legal y licencia

Proyecto de aficionados sin afiliaciÃ³n ni respaldo de Nintendo, Intelligent Systems o Koei Tecmo. *Fire Emblem*, sus personajes y emblemas pertenecen a sus respectivos titulares.

El cÃ³digo conserva la **[licencia MIT](LICENSE)** original y los avisos de copyright de Peiikai y onerag90. Las dependencias mantienen sus licencias. Deben distribuirse los avisos y el paquete de fuentes correspondientes junto al binario, incluidos los fuentes de FFmpeg y Unicorn. ConsultÃ¡ [THIRD_PARTY.md](THIRD_PARTY.md).

El mantenedor proporcionÃ³ el icono para esta distribuciÃ³n. La licencia MIT del cÃ³digo no cubre automÃ¡ticamente la imagen ni concede derechos sobre sus personajes o emblemas. El fondo opcional del lanzador se genera localmente desde los archivos del usuario.
