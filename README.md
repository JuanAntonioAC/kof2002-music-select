# KOF 2002 Music Select

Mod para **The King of Fighters 2002 Unlimited Match** (Steam, x64). Es el mismo selector de
música que `D:\KOF98-Music-Select` (comparten `library.c`, `menu.c` y `test_bgm.c`); su
`README.md` explica los controles, los álbumes y la carpeta `Music`.

Diferencias con KOF 98:

- Las canciones deben ser OGG Vorbis estéreo a **48000 Hz**. TrackPrep (`--out2002`) las
  convierte y escribe el `.loop` (segundos desde el inicio, con seis decimales).
- Una canción de `Music` sin `.loop` al lado vuelve al principio al terminar.
- La previa usa la pista `BGM15` del juego como portadora.
- Registro: `kof2002bgm.log`; última elección: `kof2002bgm.ini` (junto a la DLL).

## Instalar

- **Windows:** copia `dist\Kof2002Bgm.dll` junto a `KofVsLauncher.exe` del selector de
  controles y usa `Jugar KOF2002 con selector.bat`.
- **Steam Deck:** copia `Kof2002Bgm.dll` junto al `.exe` del juego con el cargador
  `xinput1_4.dll` (ver `D:\KOF-SteamDeck\LEEME.txt`).

## Compilar

```bat
build.bat
```

Compila las pruebas (`build\BgmSelectTest.exe`), las ejecuta y, si pasan, enlaza
`dist\Kof2002Bgm.dll`.
