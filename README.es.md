# LLM Fight Tracker

[![tests](https://github.com/alvarollopezgarcia-cpu/llm-fight-tracker/actions/workflows/tests.yml/badge.svg)](https://github.com/alvarollopezgarcia-cpu/llm-fight-tracker/actions/workflows/tests.yml)
&nbsp;[Read in English](README.md)

**Un sistema de telemetría que aprende cómo combate un jugador en un juego de Game Boy Advance y hace que los jefes finales respondan: la IA rival se adapta a los hábitos registrados del jugador, y un LLM planifica la estrategia de toda la Liga Pokémon contra él antes de que empiece.**

[![Vídeo de demostración](https://img.youtube.com/vi/8i62X_mBEUw/maxresdefault.jpg)](https://youtu.be/8i62X_mBEUw)

> 🎬 **Vídeo de demostración:** https://youtu.be/8i62X_mBEUw

Lo construí para mi [ROM hack de Pokémon Esmeralda, EmeraldFF](https://github.com/alvarollopezgarcia-cpu/emeraldFF-romhack), un proyecto en C basado en [pokeemerald-expansion](https://github.com/rh-hideout/pokeemerald-expansion). Es una herramienta de desarrollo y de portfolio: la versión pública del hack se compila con ella desactivada y no la incluye.

---

## Qué hace

1. **Registra** cada combate contra entrenadores: rival, lugar, resultado y 43 hábitos de comportamiento que el juego calcula turno a turno (¿cambia de Pokémon cuando le amenazan? ¿se potencia cuando es arriesgado? ¿abre con un Pokémon que contrarresta al equipo rival documentado?).
2. **Apunta lo que no se ve leyendo la RAM**: movimientos y su resultado, KOs, cambios, Caramelos Raros, el tope de nivel y las visitas al Centro Pokémon. El juego los escribe en un buffer circular en memoria que nunca lo bloquea.
3. **Guarda y analiza** todo en SQLite: lo que aportó cada combate, tendencias a largo plazo y resúmenes escritos por un motor de reglas gratuito o por Claude (API de Anthropic).
4. **Escribe de vuelta en el juego**: un informe en español aparece en el menú PokéNav, y el Campeón saluda al jugador con una frase sobre sus hábitos reales.
5. **Adapta la IA rival** (`AI_FLAG_PLAYER_PROFILE`, en C): el Alto Mando y el Campeón castigan los hábitos claros del jugador con pequeños ajustes de puntuación (Mofa a quien se potencia, Vasta Guardia contra ataques en área, predecir cambios con la tasa real del jugador) y cambian su lead documentado si el jugador suele contrarrestarlo. La IA solo usa información que el juego base ya le da.
6. **Planifica la Liga con un LLM**: al entrar en la Liga Pokémon, Claude lee el perfil, el historial de combates y los equipos rivales documentados, y devuelve un plan acotado para cada entrenador (leads, predicción de cambios, qué hábitos castigar, objetivo, agresividad). Python valida cada valor y, ante cualquier error, usa un plan por reglas; el juego lo aplica como pesos. [Un plan de ejemplo.](docs/examples/league_plan_claude.txt)

```
 ROM de GBA (C)             mGBA (Lua)                    Puente (Python)                    API de Claude
┌──────────────────┐  RAM  ┌──────────────┐  JSON / TCP  ┌───────────────────────┐  HTTPS  ┌──────────┐
│ hábitos          │──────►│ tracker.lua  │─────────────►│ servidor asyncio      │────────►│ resumen, │
│ buffer circular  │       │ lee cada     │              │ SQLite                │◄────────│ textos   │
│ (64 × 8 bytes)   │       │ frame, nunca │◄─────────────│ hechos → reglas/Claude│         └──────────┘
│ buzón (textos,   │◄──────│ bloquea      │   MAILBOX    │ codificador de texto  │
│ plan de la Liga) │escribe└──────────────┘   líneas     │ (charmap, anchos)     │
│ IA por perfil    │                                     │ planificador de Liga  │
└──────────────────┘                                     └───────────────────────┘
```

## Decisiones técnicas destacadas

- **Un fallo de alineación descubierto con datos reales.** La primera sesión en vivo decodificó números imposibles: tres leads en un combate y un hábito de dobles en un combate individual. Los bytes en bruto mostraban cada contador seguido de dos ceros. La ROM se compila con `-mabi=apcs-gnu`, que redondea el tamaño de todo struct a 4 bytes, así que `{u8 hits; u8 chances;}` ocupa 4 y no 2. El generador reproduce ahora esa regla; un test de regresión decodifica los bytes exactos capturados en esa sesión, y los structs nuevos en C llevan `STATIC_ASSERT` sobre su tamaño.
- **El juego nunca espera a las herramientas.** La telemetría es un buffer circular de 64 entradas: el juego solo añade y sube un contador; el lector lleva su propio cursor y cuenta como perdido lo que se sobrescribe. El script de Lua guarda los eventos en cola si el puente no está y se reconecta solo. Detecta save states y reinicios (el contador de frames retrocede) y descarta los combates a medias.
- **Coste cero en la versión pública.** Un único flag (`TELEMETRY_ENABLED`) elimina todos los enganches al compilar: las macros envuelven `if (FALSE)`, los buffers no existen, la opción de menú se oculta y el generador escribe direcciones `nil` para que el script las ignore.
- **Código de unión generado, no escrito a mano.** Las direcciones de los símbolos cambian en cada compilación, así que `gen_tracker.py` lee el mapa del enlazador y las cabeceras de C (enums con alias y miembros generados por macros, `#define`, el charmap, los anchos de la fuente) y genera tanto el script de Lua como las tablas de decodificación del puente. Lua solo copia bytes; Python, con tests, los interpreta.
- **Texto que cabe en una caja de mensajes de 2004.** Los textos para el juego se codifican con el mapa de caracteres del propio juego y se reparten en líneas por ancho en píxeles, con la tabla de anchos de la fuente y los códigos de control del motor para salto de línea, desplazamiento y caja nueva.
- **El LLM planifica; nunca juega turnos.** Una llamada al LLM tarda segundos y un turno no puede esperar, así que el modelo hace un plan estratégico por intento de Liga, al entrar en el edificio. Su salida es un esquema JSON de valores acotados (huecos del equipo, una probabilidad 0-100, pesos de -3 a 3, reglas con nombre), validado en Python contra los equipos documentados (huecos válidos, distintos, nunca el as) y codificado como un struct de C de 104 bytes que lee la IA. Una respuesta incorrecta o un fallo de la API recurre a un plan por reglas determinista, y sin puente la IA funciona solo con el perfil registrado. Cada combate de la Liga queda ligado al plan con el que se jugó, para comparar planes por reglas y planes del LLM por resultados.
- **Coste del LLM bajo control.** Por defecto, un motor de reglas determinista resume gratis tras cada combate. Claude se usa bajo demanda (unos 0,02 $ por resumen de combate con Claude Opus 5.5 y esfuerzo bajo). Ambos leen los mismos datos, y cada resumen se guarda con la entrada exacta que recibió. Claude devuelve los dos textos del juego en una sola llamada mediante un esquema JSON (salida estructurada), y lo que escribe no lo pisa la actualización automática por reglas.

## Estructura del repositorio

| Ruta | Qué es |
|---|---|
| `gen_tracker.py` | Lee el `pokeemerald.map` de una compilación y las cabeceras de C; genera el script de Lua y `game_data.json` |
| `lua/tracker_template.lua` | Script de mGBA: lectura de RAM por frame, lector del buffer circular, cliente TCP, escritura del buzón |
| `bridge/` | Python: servidor asyncio, almacenamiento SQLite, decodificación, hechos, resúmenes (reglas / Claude), buzón, planificador de la Liga |
| `tests/` | 51 tests unitarios y de integración (sockets, SQLite, un cliente de Anthropic simulado); `fixtures/` contiene copias recortadas de los archivos del juego que leen |
| `rom/` | El código C que escribí en el juego: registro de hábitos, IA basada en el perfil, telemetría y buzón, 27 tests de combate dentro del motor, más `HOOKS.md` con cada punto donde toca el motor |
| `docs/examples/` | Un plan de Liga real escrito por Claude |
| `docs/ARCHITECTURE.md` | Documentación técnica completa (en inglés): protocolo, estructura de datos, tabla de eventos, decisiones de diseño |

Unas 2.900 líneas de Python y Lua, 2.900 de C y 1.500 de tests (Python y tests de combate dentro del motor).

## Ejecutar los tests

```bash
python -m unittest discover tests
```

Solo biblioteca estándar (Python 3.10+). Se ejecutan en cada push con GitHub Actions.

## Usarlo de verdad

Necesita el código del hack (para compilar la ROM), [mGBA](https://mgba.io) 0.10+ y Python 3. Los pasos están en [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#running-it). En resumen:

```bash
python3 gen_tracker.py --repo ruta/al/hack       # después de compilar la ROM
python3 -m bridge.server --planner claude       # y cargar build/tracker.lua en mGBA
python3 -m bridge.summarize --backend claude     # opcional: requiere el paquete anthropic y una clave
```

## Lo que no está aquí

Ni ROM, ni partidas guardadas, ni recursos del juego. Pokémon es © Nintendo / Game Freak / Creatures; este repositorio solo contiene mi código y fragmentos breves de las cabeceras de la decompilación de código abierto, usados como fixtures de tests.

## Tecnologías

C (GBA, `arm-none-eabi-gcc`), Lua 5.4 (API de scripting de mGBA), Python 3 (asyncio, sqlite3, unittest), API de Claude de Anthropic (salidas estructuradas), GitHub Actions.
