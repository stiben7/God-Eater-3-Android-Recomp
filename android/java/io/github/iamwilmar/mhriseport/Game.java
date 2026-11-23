package io.github.iamwilmar.mhriseport;

// What identifies this game in the user's files: the rest of the start screen is the same in every port.
// Lo que distingue a este juego en los archivos del usuario: el resto de la pantalla de inicio es igual en cada port.
final class Game {
    static final String NAME = "God Eater 3";
    static final long APP_ID = 0x01001C700873E000L;
    // The port only works with this exact version (its addresses and patches are those of this main): recognised by the
    // build ID of the ExeFS main (bytes 0x40-0x53 of the NSO header).
    static final String VERSION = "1.0.0 (base game)";
    static final String BUILD_ID = "3887159ca9306230fcd177d9dd6a35e800000000";
    static final String ROMFS_MARKER = "resource/common/boot_config.ini";  // relative to the RomFS root
    static final String UPDATE_MARKER = null;  // update marker not checked
    static final String DLC_MARKER = null;  // DLC support not set up yet

    private Game() {}
}
