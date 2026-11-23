#include "localization.h"

#include <switch.h>

#include <algorithm>
#include <array>
#include <cctype>

namespace
{
struct Translation
{
  const char* en;
  const char* fr;
  const char* de;
  const char* es;
  const char* it;
  const char* pt;
  const char* zhHans;
  const char* zhHant;
};

constexpr std::array<LauncherLanguage, 9> s_languages{{
    {"system", "System"}, {"en", "English"}, {"fr", "Français"},
    {"de", "Deutsch"},   {"es", "Español"}, {"it", "Italiano"},
    {"pt", "Português"}, {"zh-Hans", "简体中文"},
    {"zh-Hant", "繁體中文"},
}};

// Only short, unambiguous launcher strings live here.  The detailed PS2 setting
// explanations intentionally remain their authored English text until an exact
// translation exists; a generic translated sentence is worse than useful help.
constexpr Translation s_text[] = {
    {"System", "Système", "System", "Sistema", "Sistema", "Sistema","系统","系統"},
    {"Language", "Langue", "Sprache", "Idioma", "Lingua", "Idioma","语言","語言"},
    {"Settings", "Paramètres", "Einstellungen", "Ajustes", "Impostazioni", "Definições","设置","設定"},
    {"Launcher", "Lanceur", "Launcher", "Lanzador", "Launcher", "Iniciador","启动器","啟動器"},
    {"Library & storage", "Bibliothèque et stockage", "Bibliothek & Speicher", "Biblioteca y almacenamiento", "Libreria e archiviazione", "Biblioteca e armazenamento","游戏库与存储","遊戲庫與儲存"},
    {"Graphics", "Graphismes", "Grafik", "Gráficos", "Grafica", "Gráficos","图形","圖形"},
    {"Display", "Affichage", "Anzeige", "Pantalla", "Schermo", "Ecrã", "显示", "顯示"},
    {"Rendering", "Rendu", "Rendering", "Renderizado", "Rendering", "Renderização", "渲染", "渲染"},
    {"Textures", "Textures", "Texturen", "Texturas", "Texture", "Texturas", "贴图", "貼圖"},
    {"Advanced graphics", "Graphismes avancés", "Erweiterte Grafik", "Gráficos avanzados", "Grafica avanzata", "Gráficos avançados", "高级图形", "進階圖形"},
    {"Manual graphics fixes", "Correctifs graphiques manuels", "Manuelle Grafikkorrekturen", "Correcciones gráficas manuales", "Correzioni grafiche manuali", "Correções gráficas manuais", "手动图形修复", "手動圖形修復"},
    {"Widescreen patches", "Patchs écran large", "Breitbild-Patches", "Parches de pantalla panorámica", "Patch widescreen", "Patches de ecrã panorâmico", "宽屏补丁", "寬螢幕修補程式"},
    {"No-interlace patches", "Patchs anti-entrelacement", "Patches gegen Zeilensprung", "Parches sin entrelazado", "Patch anti-interlacciamento", "Patches sem entrelaçamento", "去隔行补丁", "去交錯修補程式"},
    {"Load texture replacements", "Charger les textures de remplacement", "Ersatztexturen laden", "Cargar texturas de reemplazo", "Carica texture sostitutive", "Carregar texturas de substituição","加载替换贴图","載入替換貼圖"},
    {"Asynchronous texture loading", "Chargement asynchrone des textures", "Asynchrones Laden von Texturen", "Carga asíncrona de texturas", "Caricamento asincrono delle texture", "Carregamento assíncrono de texturas","异步贴图加载","非同步貼圖載入"},
    {"Audio", "Audio", "Audio", "Audio", "Audio", "Áudio","音频","音訊"},
    {"Emulation", "Émulation", "Emulation", "Emulación", "Emulazione", "Emulação", "模拟", "模擬"},
    {"Automatic game patches", "Patchs de jeu automatiques", "Automatische Spiel-Patches", "Parches automáticos del juego", "Patch di gioco automatiche", "Patches automáticos do jogo", "自动游戏补丁", "自動遊戲修補程式"},
    {"Cheats", "Codes de triche", "Cheats", "Trucos", "Trucchi", "Batotas", "金手指", "金手指"},
    {"CPU emulation", "Émulation CPU", "CPU-Emulation", "Emulación de CPU", "Emulazione CPU", "Emulação da CPU", "CPU 模拟", "CPU 模擬"},
    {"Advanced CPU settings", "Paramètres CPU avancés", "Erweiterte CPU-Einstellungen", "Ajustes avanzados de CPU", "Impostazioni CPU avanzate", "Definições avançadas da CPU", "高级 CPU 设置", "進階 CPU 設定"},
    {"Advanced", "Avancé", "Erweitert", "Avanzado", "Avanzate", "Avançado","高级","進階"},
    {"Manual game fixes", "Correctifs de jeu manuels", "Manuelle Spielkorrekturen", "Correcciones manuales del juego", "Correzioni manuali del gioco", "Correções manuais do jogo", "手动游戏修复", "手動遊戲修復"},
    {"Speed - Frame pacing", "Vitesse - Cadence d'images", "Geschwindigkeit - Bildtakt", "Velocidad - Ritmo de fotogramas", "Velocità - Cadenza dei fotogrammi", "Velocidade - Ritmo de fotogramas", "速度 - 帧节奏", "速度 - 幀節奏"},
    {"Controller", "Manette", "Controller", "Mando", "Controller", "Comando","手柄","手把"},
    {"Game settings", "Paramètres du jeu", "Spieleinstellungen", "Ajustes del juego", "Impostazioni di gioco", "Definições do jogo","游戏设置","遊戲設定"},
    {"Global settings", "Paramètres globaux", "Globale Einstellungen", "Ajustes globales", "Impostazioni globali", "Definições globais","全局设置","全域設定"},
    {"Global setting", "Paramètre global", "Globale Einstellung", "Ajuste global", "Impostazione globale", "Definição global","全局设置","全域設定"},
    {"Per-game setting", "Paramètre par jeu", "Spieleinstellung", "Ajuste por juego", "Impostazione per gioco", "Definição por jogo","单游戏设置","單遊戲設定"},
    {"Launcher setting", "Paramètre du lanceur", "Launcher-Einstellung", "Ajuste del lanzador", "Impostazione launcher", "Definição do iniciador","启动器设置","啟動器設定"},
    {"Settings category", "Catégorie de paramètres", "Einstellungskategorie", "Categoría de ajustes", "Categoria impostazioni", "Categoria de definições","设置分类","設定分類"},
    {"Setting", "Paramètre", "Einstellung", "Ajuste", "Impostazione", "Definição","设置项","設定項"},
    {"Setting help", "Aide du paramètre", "Einstellungshilfe", "Ayuda del ajuste", "Aiuto impostazione", "Ajuda da definição","设置说明","設定說明"},
    {"Current: ", "Actuel : ", "Aktuell: ", "Actual: ", "Attuale: ", "Atual: ","当前： ","目前： "},
    {"On", "Activé", "Ein", "Activado", "Attivo", "Ligado","开","開"},
    {"CPU Boost", "Boost CPU", "CPU-Boost", "Aceleración de CPU", "Boost CPU", "Boost da CPU", "CPU 加速", "CPU 加速"},
    {"Higher CPU clock, lower GPU clock", "Fréquence CPU plus élevée, fréquence GPU réduite", "Höherer CPU-Takt, niedrigerer GPU-Takt", "Mayor frecuencia de CPU, menor frecuencia de GPU", "Frequenza CPU più alta, frequenza GPU più bassa", "Frequência de CPU mais alta, frequência de GPU mais baixa", "提高 CPU 频率，降低 GPU 频率", "提高 CPU 頻率，降低 GPU 頻率"},
    {"Console clocks", "Fréquences de la console", "Konsolentakt", "Frecuencias de la consola", "Frequenze della console", "Frequências da consola", "主机频率", "主機頻率"},
    {"Keeps CPU boost active throughout the game. It raises the CPU clock and lowers the GPU clock. The quick menu can change it for the current session.", "Maintient le boost CPU actif pendant toute la partie. Il augmente la fréquence du CPU et réduit celle du GPU. Le menu rapide permet de le modifier pour la session en cours.", "Hält den CPU-Boost während des gesamten Spiels aktiv. Er erhöht den CPU-Takt und senkt den GPU-Takt. Im Schnellmenü lässt er sich für die laufende Sitzung ändern.", "Mantiene activa la aceleración de CPU durante toda la partida. Aumenta la frecuencia de CPU y reduce la de GPU. El menú rápido permite cambiarla para la sesión actual.", "Mantiene attivo il boost CPU per tutta la partita. Aumenta la frequenza della CPU e riduce quella della GPU. Il menu rapido consente di cambiarlo per la sessione corrente.", "Mantém o boost da CPU ativo durante todo o jogo. Aumenta a frequência da CPU e reduz a da GPU. O menu rápido permite alterá-lo para a sessão atual.", "在整个游戏期间保持 CPU 加速，提高 CPU 频率并降低 GPU 频率。可在快捷菜单中为当前游戏调整。", "在整個遊戲期間保持 CPU 加速，提高 CPU 頻率並降低 GPU 頻率。可在快捷選單中為目前遊戲調整。"},
    {"Off", "Désactivé", "Aus", "Desactivado", "Disattivato", "Desligado","关","關"},
    {"Yes", "Oui", "Ja", "Sí", "Sì", "Sim","是","是"},
    {"No", "Non", "Nein", "No", "No", "Não","否","否"},
    {"None", "Aucun", "Keine", "Ninguno", "Nessuno", "Nenhum","无","無"},
    {"Choose", "Choisir", "Auswählen", "Elegir", "Scegli", "Escolher","选择","選擇"},
    {"Change", "Modifier", "Ändern", "Cambiar", "Cambia", "Alterar","更改","變更"},
    {"Toggle", "Basculer", "Umschalten", "Alternar", "Attiva/Disattiva", "Alternar","切换","切換"},
    {"Increase", "Augmenter", "Erhöhen", "Aumentar", "Aumenta", "Aumentar","增加","增加"},
    {"Open", "Ouvrir", "Öffnen", "Abrir", "Apri", "Abrir","打开","開啟"},
    {"Back", "Retour", "Zurück", "Atrás", "Indietro", "Voltar","返回","返回"},
    {"Close", "Fermer", "Schließen", "Cerrar", "Chiudi", "Fechar","关闭","關閉"},
    {"Continue", "Continuer", "Weiter", "Continuar", "Continua", "Continuar","继续","繼續"},
    {"Reset", "Réinitialiser", "Zurücksetzen", "Restablecer", "Ripristina", "Repor","重置","重設"},
    {"Setting reset to default", "Paramètre réinitialisé à sa valeur par défaut", "Einstellung auf Standardwert zurückgesetzt", "Ajuste restablecido al valor predeterminado", "Impostazione ripristinata al valore predefinito", "Definição reposta para o valor predefinido","设置已重置为默认值","設定已重設為預設值"},
    {"Info", "Infos", "Info", "Info", "Info", "Info","信息","資訊"},
    {"Launch", "Lancer", "Starten", "Iniciar", "Avvia", "Iniciar","启动","啟動"},
    {"Game Menu", "Menu du jeu", "Spielmenü", "Menú del juego", "Menu gioco", "Menu do jogo","游戏菜单","遊戲選單"},
    {"Page", "Page", "Seite", "Página", "Pagina", "Página","页","頁"},
    {"Quit", "Quitter", "Beenden", "Salir", "Esci", "Sair","退出","退出"},
    {"All games", "Tous les jeux", "Alle Spiele", "Todos los juegos", "Tutti i giochi", "Todos os jogos","全部游戏","全部遊戲"},
    {"Recent", "Récents", "Zuletzt gespielt", "Recientes", "Recenti", "Recentes", "最近游玩", "最近遊玩"},
    {"Nothing played yet", "Aucun jeu lancé", "Noch nichts gespielt", "Todavía no has jugado", "Nessun gioco avviato", "Ainda não jogou", "尚未游玩", "尚未遊玩"},
    {"No favorites yet", "Aucun favori", "Noch keine Favoriten", "Aún no hay favoritos", "Nessun preferito", "Ainda sem favoritos", "暂无收藏", "尚無收藏"},
    {"Games you launch appear here, most recent first.", "Les jeux lancés apparaissent ici, du plus récent au plus ancien.", "Gestartete Spiele erscheinen hier, die neuesten zuerst.", "Tus juegos aparecerán aquí, los más recientes primero.", "I giochi avviati appaiono qui, dal più recente.", "Os jogos iniciados aparecem aqui, do mais recente.", "启动过的游戏会显示在这里，最近游玩的排在前面。", "啟動過的遊戲會顯示於此，最近遊玩的排在前面。"},
    {"Mark games as favorites from their game menu.", "Ajoutez des favoris depuis le menu de chaque jeu.", "Markiere Favoriten im Menü des jeweiligen Spiels.", "Añade favoritos desde el menú de cada juego.", "Aggiungi preferiti dal menu di ciascun gioco.", "Adicione favoritos no menu de cada jogo.", "在游戏菜单中将游戏添加到收藏。", "在遊戲選單中將遊戲加入收藏。"},
    {"Add game folders in Settings to populate your library.", "Ajoutez des dossiers de jeux dans les paramètres.", "Füge in den Einstellungen Spieleordner hinzu.", "Añade carpetas de juegos en Ajustes.", "Aggiungi cartelle di giochi nelle impostazioni.", "Adicione pastas de jogos nas definições.", "在设置中添加游戏文件夹来填充游戏库。", "在設定中新增遊戲資料夾來填入遊戲庫。"},
    {"All artwork already downloaded", "Toutes les illustrations sont déjà téléchargées", "Alle Bilder bereits heruntergeladen", "Ya se han descargado todas las imágenes", "Tutte le immagini sono già scaricate", "Todas as imagens já foram descarregadas", "所有图片均已下载", "所有圖片均已下載"},
    {"Downloading artwork", "Téléchargement des illustrations", "Bilder werden heruntergeladen", "Descargando imágenes", "Download delle immagini", "A descarregar imagens", "正在下载图片", "正在下載圖片"},
    {"Artwork: %d updated, %d failed%s", "Illustrations : %d mises à jour, %d échecs%s", "Bilder: %d aktualisiert, %d fehlgeschlagen%s", "Imágenes: %d actualizadas, %d fallidas%s", "Immagini: %d aggiornate, %d non riuscite%s", "Imagens: %d atualizadas, %d falhadas%s", "图片：%d 个已更新，%d 个失败%s", "圖片：%d 個已更新，%d 個失敗%s"},
    {" (cancelled)", " (annulé)", " (abgebrochen)", " (cancelado)", " (annullato)", " (cancelado)", "（已取消）", "（已取消）"},
    {"Downloads missing covers and Classic backgrounds from SteamGridDB. Existing local artwork is kept.", "Télécharge les jaquettes et fonds Classic manquants sur SteamGridDB. Les illustrations locales sont conservées.", "Lädt fehlende Cover und Classic-Hintergründe von SteamGridDB. Vorhandene lokale Bilder bleiben erhalten.", "Descarga carátulas y fondos Classic que falten de SteamGridDB. Conserva las imágenes locales existentes.", "Scarica da SteamGridDB le copertine e gli sfondi Classic mancanti. Mantiene le immagini locali esistenti.", "Descarrega as capas e fundos Classic em falta do SteamGridDB. As imagens locais existentes são mantidas.", "从 SteamGridDB 下载缺失的封面和 Classic 背景，保留现有本地图片。", "從 SteamGridDB 下載缺少的封面和 Classic 背景，保留現有本機圖片。"},
    {"Favorites", "Favoris", "Favoriten", "Favoritos", "Preferiti", "Favoritos","收藏","收藏"},
    {"Favorite", "Favori", "Favorit", "Favorito", "Preferito", "Favorito","收藏","收藏"},
    {"Add game folder", "Ajouter un dossier de jeux", "Spieleordner hinzufügen", "Añadir carpeta de juegos", "Aggiungi cartella giochi", "Adicionar pasta de jogos","添加游戏文件夹","新增遊戲資料夾"},
    {"Game folders", "Dossiers de jeux", "Spieleordner", "Carpetas de juegos", "Cartelle giochi", "Pastas de jogos","游戏文件夹","遊戲資料夾"},
    {"File manager", "Gestionnaire de fichiers", "Dateimanager", "Gestor de archivos", "Gestore file", "Gestor de ficheiros","文件管理器","檔案管理員"},
    {"Download covers", "Télécharger les jaquettes", "Cover herunterladen", "Descargar carátulas", "Scarica copertine", "Transferir capas","下载封面","下載封面"},
    {"Artwork", "Illustration", "Bild", "Imagen", "Immagine", "Imagem", "图片", "圖片"},
    {"Choose background artwork", "Choisir une illustration de fond", "Hintergrundbild auswählen", "Elegir imagen de fondo", "Scegli uno sfondo", "Escolher imagem de fundo", "选择背景图片", "選擇背景圖片"},
    {"Loading background artwork", "Chargement des illustrations de fond", "Hintergrundbilder werden geladen", "Cargando imágenes de fondo", "Caricamento degli sfondi", "A carregar imagens de fundo", "正在加载背景图片", "正在載入背景圖片"},
    {"Downloading selected background", "Téléchargement du fond choisi", "Ausgewählter Hintergrund wird heruntergeladen", "Descargando el fondo elegido", "Download dello sfondo selezionato", "A descarregar o fundo escolhido", "正在下载所选背景", "正在下載所選背景"},
    {"Skip background", "Passer le fond", "Hintergrund überspringen", "Omitir fondo", "Salta sfondo", "Ignorar fundo", "跳过背景", "略過背景"},
    {"Cover downloaded. No background artwork was found.", "Jaquette téléchargée. Aucune illustration de fond trouvée.", "Cover heruntergeladen. Kein Hintergrundbild gefunden.", "Carátula descargada. No se encontró ninguna imagen de fondo.", "Copertina scaricata. Nessuno sfondo trovato.", "Capa descarregada. Não foi encontrada uma imagem de fundo.", "封面已下载，未找到背景图片。", "封面已下載，未找到背景圖片。"},
    {"Cover and background downloaded", "Jaquette et fond téléchargés", "Cover und Hintergrund heruntergeladen", "Carátula y fondo descargados", "Copertina e sfondo scaricati", "Capa e fundo descarregados", "封面和背景已下载", "封面與背景已下載"},
    {"Background search failed", "Échec de la recherche de fonds", "Suche nach Hintergrund fehlgeschlagen", "Falló la búsqueda de fondos", "Ricerca sfondi non riuscita", "Falha ao procurar fundos", "背景搜索失败", "背景搜尋失敗"},
    {"Background download failed", "Échec du téléchargement du fond", "Hintergrund konnte nicht heruntergeladen werden", "Falló la descarga del fondo", "Download dello sfondo non riuscito", "Falha ao descarregar o fundo", "背景下载失败", "背景下載失敗"},
    {"Cover settings", "Paramètres de la jaquette", "Cover-Einstellungen", "Ajustes de la carátula", "Impostazioni copertina", "Definições da capa","封面设置","封面設定"},
    {"Download from SteamGridDB", "Télécharger depuis SteamGridDB", "Von SteamGridDB herunterladen", "Descargar desde SteamGridDB", "Scarica da SteamGridDB", "Transferir do SteamGridDB","从 SteamGridDB 下载","從 SteamGridDB 下載"},
    {"Import cover from file", "Importer une jaquette depuis un fichier", "Cover aus Datei importieren", "Importar carátula desde un archivo", "Importa copertina da file", "Importar capa de um ficheiro","从文件导入封面","從檔案匯入封面"},
    {"Online artwork", "Illustration en ligne", "Online-Cover", "Ilustración en línea", "Immagine online", "Imagem online","在线封面","線上封面"},
    {"Local image", "Image locale", "Lokales Bild", "Imagen local", "Immagine locale", "Imagem local","本地图片","本機圖片"},
    {"Choose a game cover, then select optional background artwork for the Classic theme.", "Choisissez une jaquette, puis une illustration de fond facultative pour le thème Classic.", "Wähle ein Spielcover und anschließend optional ein Hintergrundbild für das Classic-Design.", "Elige una carátula y después, si quieres, una imagen de fondo para el tema Classic.", "Scegli una copertina, poi uno sfondo facoltativo per il tema Classic.", "Escolha uma capa e depois, se quiser, uma imagem de fundo para o tema Classic.", "选择游戏封面后，可为 Classic 主题选择背景图片。", "選擇遊戲封面後，可為 Classic 主題選擇背景圖片。"},
    {"Choose a PNG, JPEG, WebP or BMP image from the SD card. It is validated and saved safely as PNG.", "Choisissez une image PNG, JPEG, WebP ou BMP sur la carte SD. Elle est vérifiée et enregistrée en toute sécurité au format PNG.", "Wähle ein PNG-, JPEG-, WebP- oder BMP-Bild von der SD-Karte. Es wird geprüft und sicher als PNG gespeichert.", "Elige una imagen PNG, JPEG, WebP o BMP de la tarjeta SD. Se valida y guarda de forma segura como PNG.", "Scegli un'immagine PNG, JPEG, WebP o BMP dalla scheda SD. Verrà verificata e salvata in modo sicuro come PNG.", "Escolha uma imagem PNG, JPEG, WebP ou BMP do cartão SD. É validada e guardada em segurança como PNG.", "从 SD 卡中选择 PNG、JPEG、WebP 或 BMP 图片。图片会经过验证并安全保存为 PNG。", "從 SD 卡中選擇 PNG、JPEG、WebP 或 BMP 圖片。圖片會經過驗證並安全儲存為 PNG。"},
    {"Remove custom cover", "Supprimer la jaquette personnalisée", "Benutzerdefiniertes Cover entfernen", "Quitar carátula personalizada", "Rimuovi copertina personalizzata", "Remover capa personalizada","移除自定义封面","移除自訂封面"},
    {"Select local cover", "Sélectionner une jaquette locale", "Lokales Cover auswählen", "Seleccionar carátula local", "Seleziona copertina locale", "Selecionar capa local","选择本地封面","選擇本機封面"},
    {"Cover imported", "Jaquette importée", "Cover importiert", "Carátula importada", "Copertina importata", "Capa importada","封面已导入","封面已匯入"},
    {"Custom cover removed", "Jaquette personnalisée supprimée", "Benutzerdefiniertes Cover entfernt", "Carátula personalizada eliminada", "Copertina personalizzata rimossa", "Capa personalizada removida","自定义封面已移除","自訂封面已移除"},
    {"Check for updates", "Rechercher des mises à jour", "Nach Updates suchen", "Buscar actualizaciones", "Controlla aggiornamenti", "Procurar atualizações","检查更新","檢查更新"},
    {"Theme", "Thème", "Design", "Tema", "Tema", "Tema","主题","主題"},
    {"Games per row", "Jeux par ligne", "Spiele pro Zeile", "Juegos por fila", "Giochi per riga", "Jogos por linha","每行游戏数","每列遊戲數"},
    {"Rows per page", "Lignes par page", "Zeilen pro Seite", "Filas por página", "Righe per pagina", "Linhas por página","每页行数","每頁列數"},
    {"Show game titles", "Afficher les titres des jeux", "Spieltitel anzeigen", "Mostrar títulos de juegos", "Mostra i titoli dei giochi", "Mostrar títulos dos jogos","显示游戏标题","顯示遊戲標題"},
    {"Show region flags", "Afficher les drapeaux de région", "Regionsflaggen anzeigen", "Mostrar banderas de región", "Mostra bandiere regionali", "Mostrar bandeiras de região","显示区域标识","顯示區域旗幟"},
    {"Show custom settings badges", "Afficher les indicateurs de paramètres personnalisés", "Markierungen für benutzerdefinierte Einstellungen anzeigen", "Mostrar indicadores de ajustes personalizados", "Mostra indicatori delle impostazioni personalizzate", "Mostrar indicadores de definições personalizadas","显示自定义设置标记","顯示自訂設定標記"},
    {"UI animations", "Animations de l'interface", "UI-Animationen", "Animaciones de la interfaz", "Animazioni dell'interfaccia", "Animações da interface","界面动画","介面動畫"},
    {"Sound effects", "Effets sonores", "Soundeffekte", "Efectos de sonido", "Effetti sonori", "Efeitos sonoros","音效","音效"},
    {"Check updates at boot", "Vérifier au démarrage", "Beim Start nach Updates suchen", "Buscar actualizaciones al iniciar", "Controlla aggiornamenti all'avvio", "Procurar atualizações ao iniciar","启动时检查更新","啟動時檢查更新"},
    {"Emulator", "Émulateur", "Emulator", "Emulador", "Emulatore", "Emulador","模拟器","模擬器"},
    {"Loading game library...", "Chargement de la bibliothèque...", "Spielebibliothek wird geladen...", "Cargando la biblioteca...", "Caricamento della libreria...", "A carregar a biblioteca...","正在加载游戏库...","正在載入遊戲庫..."},
    {"The first page will appear as soon as it is ready.", "La première page s'affichera dès qu'elle sera prête.", "Die erste Seite erscheint, sobald sie bereit ist.", "La primera página aparecerá en cuanto esté lista.", "La prima pagina apparirà non appena sarà pronta.", "A primeira página aparecerá assim que estiver pronta.","第一页准备好后会立即显示。","第一頁準備好後會立即顯示。"},
    {"No games found", "Aucun jeu trouvé", "Keine Spiele gefunden", "No se encontraron juegos", "Nessun gioco trovato", "Nenhum jogo encontrado","未找到游戏","找不到遊戲"},
    {"NO COVER", "AUCUNE JAQUETTE", "KEIN COVER", "SIN CARÁTULA", "NESSUNA COPERTINA", "SEM CAPA","无封面","無封面"},
    {"Exit ARMSX2-nx?", "Quitter ARMSX2-nx ?", "ARMSX2-nx beenden?", "¿Salir de ARMSX2-nx?", "Uscire da ARMSX2-nx?", "Sair do ARMSX2-nx?","退出 ARMSX2-nx？","退出 ARMSX2-nx？"},
    {"Play this game with another Nintendo Switch or Nintendo Switch 2 console?", "Jouer à ce jeu avec une autre console Nintendo Switch ou Nintendo Switch 2 ?", "Dieses Spiel mit einer anderen Nintendo Switch oder Nintendo Switch 2 spielen?", "¿Jugar a este juego con otra consola Nintendo Switch o Nintendo Switch 2?", "Giocare a questo gioco con un'altra console Nintendo Switch o Nintendo Switch 2?", "Jogar este jogo com outra consola Nintendo Switch ou Nintendo Switch 2?","与另一台 Nintendo Switch 或 Nintendo Switch 2 主机一起玩这款游戏？","與另一台 Nintendo Switch 或 Nintendo Switch 2 主機一起玩這款遊戲？"},
    {"kaiHEN must be running in a game that supports GameShare.", "kaiHEN doit tourner dans un jeu compatible avec GameShare.", "kaiHEN muss in einem Spiel laufen, das GameShare unterstützt.", "kaiHEN debe ejecutarse en un juego compatible con GameShare.", "kaiHEN deve essere in esecuzione in un gioco che supporta GameShare.", "O kaiHEN tem de estar a correr num jogo compatível com GameShare.","kaiHEN 必须在支持 GameShare 的游戏中运行。","kaiHEN 必須在支援 GameShare 的遊戲中執行。"},
    {"Active scans and network operations will be cancelled safely.", "Les analyses et opérations réseau actives seront annulées en toute sécurité.", "Aktive Scans und Netzwerkvorgänge werden sicher abgebrochen.", "Los análisis y operaciones de red activos se cancelarán de forma segura.", "Le scansioni e le operazioni di rete attive verranno annullate in sicurezza.", "As análises e operações de rede ativas serão canceladas com segurança.","正在进行的扫描和网络操作将取消。","正在進行的掃描和網路操作將取消。"},
    {"Return to the Homebrew Menu?", "Retourner au menu Homebrew ?", "Zum Homebrew-Menü zurückkehren?", "¿Volver al menú Homebrew?", "Tornare al menu Homebrew?", "Voltar ao menu Homebrew?", "返回自制软件菜单？", "返回自製軟體選單？"},
    {"Closing ARMSX2-nx...", "Fermeture de ARMSX2-nx...", "ARMSX2-nx wird beendet...", "Cerrando ARMSX2-nx...", "Chiusura di ARMSX2-nx...", "A fechar o ARMSX2-nx...","正在关闭 ARMSX2-nx...","正在關閉 ARMSX2-nx..."},
    {"Finishing background operations safely.", "Finalisation sécurisée des opérations en arrière-plan.", "Hintergrundvorgänge werden sicher abgeschlossen.", "Finalizando de forma segura las operaciones en segundo plano.", "Completamento sicuro delle operazioni in background.", "A concluir as operações em segundo plano com segurança.","正在安全完成后台操作。","正在安全完成背景操作。"},
    {"Working", "Traitement", "Verarbeitung", "Procesando", "Elaborazione", "A processar","处理中","處理中"},
    {"Cancelling at the next safe point...", "Annulation au prochain point sûr...", "Abbruch am nächsten sicheren Punkt...", "Cancelando en el siguiente punto seguro...", "Annullamento al prossimo punto sicuro...", "A cancelar no próximo ponto seguro...","将在下一个安全点取消...","將在下一個安全點取消..."},
    {"Do not remove the active storage device or close ARMSX2-nx.", "Ne retirez pas le périphérique de stockage actif et ne fermez pas ARMSX2-nx.", "Das aktive Speichergerät nicht entfernen und ARMSX2-nx nicht schließen.", "No retires el dispositivo de almacenamiento activo ni cierres ARMSX2-nx.", "Non rimuovere il dispositivo di archiviazione attivo e non chiudere ARMSX2-nx.", "Não remova o dispositivo de armazenamento ativo nem feche o ARMSX2-nx.","请勿移除当前存储设备或关闭 ARMSX2-nx。","請勿移除目前儲存裝置或關閉 ARMSX2-nx。"},
    {"Custom textures", "Textures personnalisées", "Benutzerdefinierte Texturen", "Texturas personalizadas", "Texture personalizzate", "Texturas personalizadas","自定义贴图","自訂貼圖"},
    {"Display geometry", "Géométrie d'affichage", "Anzeigegeometrie", "Geometría de pantalla", "Geometria schermo", "Geometria do ecrã","显示几何","顯示幾何"},
    {"Frame pacing", "Rythme des images", "Frame-Pacing", "Ritmo de fotogramas", "Ritmo dei fotogrammi", "Ritmo dos fotogramas","帧节奏","幀節奏"},
    {"Frame pacing / latency", "Rythme / latence", "Frame-Pacing / Latenz", "Ritmo / latencia", "Ritmo / latenza", "Ritmo / latência","帧节奏 / 延迟","幀節奏 / 延遲"},
    {"Game patches", "Patchs de jeu", "Spiel-Patches", "Parches del juego", "Patch di gioco", "Patches do jogo","游戏补丁","遊戲修補程式"},
    {"Graphics backend", "Backend graphique", "Grafik-Backend", "Backend gráfico", "Backend grafico", "Backend gráfico","图形后端","圖形後端"},
    {"Launcher language", "Langue du lanceur", "Launcher-Sprache", "Idioma del lanzador", "Lingua del launcher", "Idioma do launcher","启动器语言","啟動器語言"},
    {"On-screen display", "Affichage à l'écran", "Bildschirmanzeige", "Visualización en pantalla", "Visualizzazione a schermo", "Apresentação no ecrã","屏幕显示","螢幕顯示"},
    {"Performance / latency trade-off", "Compromis performances / latence", "Leistungs-/Latenz-Abwägung", "Equilibrio rendimiento / latencia", "Compromesso prestazioni / latenza", "Equilíbrio desempenho / latência","性能 / 延迟权衡","效能 / 延遲權衡"},
    {"Performance display", "Affichage des performances", "Leistungsanzeige", "Visualización de rendimiento", "Visualizzazione prestazioni", "Apresentação de desempenho","性能显示","效能顯示"},
    {"Post-processing", "Post-traitement", "Nachbearbeitung", "Posprocesado", "Post-elaborazione", "Pós-processamento","后处理","後處理"},
    {"Resolution / performance", "Résolution / performances", "Auflösung / Leistung", "Resolución / rendimiento", "Risoluzione / prestazioni", "Resolução / desempenho","分辨率 / 性能","解析度 / 效能"},
    {"Texture filtering", "Filtrage des textures", "Texturfilterung", "Filtrado de texturas", "Filtro texture", "Filtragem de texturas","贴图过滤","貼圖過濾"},
    {"Video display", "Affichage vidéo", "Videoanzeige", "Visualización de vídeo", "Visualizzazione video", "Apresentação de vídeo","视频显示","視訊顯示"},
    {"Launcher appearance", "Apparence du lanceur", "Launcher-Darstellung", "Aspecto del lanzador", "Aspetto del launcher", "Aspeto do iniciador","启动器外观","啟動器外觀"},
    {"Library layout", "Disposition de la bibliothèque", "Bibliothekslayout", "Diseño de la biblioteca", "Layout della libreria", "Disposição da biblioteca","游戏库布局","遊戲庫版面"},
    {"Launcher audio", "Audio du lanceur", "Launcher-Audio", "Audio del lanzador", "Audio del launcher", "Áudio do iniciador","启动器音频","啟動器音訊"},
    {"Launcher updates", "Mises à jour du lanceur", "Launcher-Updates", "Actualizaciones del lanzador", "Aggiornamenti del launcher", "Atualizações do iniciador","启动器更新","啟動器更新"},
    {"Changes the launcher background and visual style. It does not affect gameplay rendering or performance inside the emulator.", "Modifie l'arrière-plan et le style visuel du lanceur. Cela n'affecte ni le rendu du jeu ni les performances de l'émulateur.", "Ändert Hintergrund und Stil des Launchers. Dies beeinflusst weder die Spieldarstellung noch die Leistung des Emulators.", "Cambia el fondo y el estilo visual del lanzador. No afecta al renderizado del juego ni al rendimiento del emulador.", "Cambia lo sfondo e lo stile visivo del launcher. Non influisce sul rendering del gioco né sulle prestazioni dell'emulatore.", "Altera o fundo e o estilo visual do iniciador. Não afeta a renderização do jogo nem o desempenho do emulador.","更改启动器背景与视觉风格。不影响模拟器内的游戏渲染或性能。","變更啟動器背景與視覺風格。不影響模擬器內的遊戲渲染或效能。"},
    {"Sets how many game covers appear across each library row. More columns make each cover smaller.", "Définit le nombre de jaquettes par ligne de la bibliothèque. Plus de colonnes réduisent la taille des jaquettes.", "Legt die Anzahl der Cover pro Bibliothekszeile fest. Mehr Spalten machen jedes Cover kleiner.", "Define cuántas carátulas aparecen en cada fila de la biblioteca. Más columnas reducen cada carátula.", "Imposta quante copertine appaiono in ogni riga della libreria. Più colonne riducono le copertine.", "Define quantas capas aparecem em cada linha da biblioteca. Mais colunas tornam cada capa menor.","设置游戏库每行显示的封面数量。列数越多，每个封面越小。","設定遊戲庫每列顯示的封面數量。欄數越多，每個封面越小。"},
    {"Sets how many cover rows appear on one library page. More rows make each cover smaller.", "Définit le nombre de lignes de jaquettes par page. Plus de lignes réduisent la taille des jaquettes.", "Legt die Anzahl der Coverzeilen pro Bibliotheksseite fest. Mehr Zeilen machen jedes Cover kleiner.", "Define cuántas filas de carátulas aparecen en una página. Más filas reducen cada carátula.", "Imposta quante righe di copertine appaiono in una pagina. Più righe riducono le copertine.", "Define quantas linhas de capas aparecem numa página. Mais linhas tornam cada capa menor.","设置游戏库每页显示的封面行数。行数越多，每个封面越小。","設定遊戲庫每頁顯示的封面列數。列數越多，每個封面越小。"},
    {"Shows or hides game names below their covers in the launcher library.", "Affiche ou masque le nom des jeux sous leur jaquette dans la bibliothèque.", "Zeigt oder verbirgt Spielnamen unter den Covern in der Launcher-Bibliothek.", "Muestra u oculta los nombres de los juegos bajo sus carátulas en la biblioteca.", "Mostra o nasconde i nomi dei giochi sotto le copertine nella libreria.", "Mostra ou oculta os nomes dos jogos por baixo das capas na biblioteca.","在启动器游戏库中显示或隐藏封面下方的游戏名称。","在啟動器遊戲庫中顯示或隱藏封面下方的遊戲名稱。"},
    {"Shows or hides the region flag in the top-left corner of each game cover.", "Affiche ou masque le drapeau de région dans le coin supérieur gauche de chaque jaquette.", "Blendet die Regionsflagge oben links auf jedem Spielcover ein oder aus.", "Muestra u oculta la bandera de región en la esquina superior izquierda de cada carátula.", "Mostra o nasconde la bandiera regionale nell'angolo superiore sinistro di ogni copertina.", "Mostra ou oculta a bandeira de região no canto superior esquerdo de cada capa.","显示或隐藏每个游戏封面左上角的区域旗帜。","顯示或隱藏每個遊戲封面左上角的區域旗幟。"},
    {"Shows or hides the square badge on games that have per-game settings. The settings themselves are not changed.", "Affiche ou masque l'indicateur carré sur les jeux ayant des paramètres par jeu. Les paramètres eux-mêmes ne sont pas modifiés.", "Blendet die quadratische Markierung bei Spielen mit spielspezifischen Einstellungen ein oder aus. Die Einstellungen selbst werden nicht geändert.", "Muestra u oculta el indicador cuadrado en los juegos con ajustes por juego. Los ajustes no se modifican.", "Mostra o nasconde l'indicatore quadrato sui giochi con impostazioni specifiche. Le impostazioni non vengono modificate.", "Mostra ou oculta o indicador quadrado nos jogos com definições por jogo. As definições não são alteradas.","显示或隐藏具有单游戏设置的游戏上的方形标记。设置本身不会改变。","顯示或隱藏具有單遊戲設定的遊戲上的方形標記。設定本身不會改變。"},
    {"Enables launcher transitions, moving selection highlights, and animated theme effects.", "Active les transitions du lanceur, les surbrillances mobiles et les effets animés du thème.", "Aktiviert Launcher-Übergänge, bewegte Auswahlmarkierungen und animierte Theme-Effekte.", "Activa las transiciones del lanzador, los resaltados de selección móviles y los efectos animados del tema.", "Attiva le transizioni del launcher, l'evidenziazione mobile e gli effetti animati del tema.", "Ativa as transições do iniciador, os realces móveis da seleção e os efeitos animados do tema.","启用启动器过渡动画、移动选中高亮与主题动画效果。","啟用啟動器過渡動畫、移動選取反白與主題動畫效果。"},
    {"Enables navigation, confirmation, and back sound effects in the launcher.", "Active les sons de navigation, de confirmation et de retour dans le lanceur.", "Aktiviert Navigations-, Bestätigungs- und Zurück-Sounds im Launcher.", "Activa los sonidos de navegación, confirmación y retroceso en el lanzador.", "Attiva i suoni di navigazione, conferma e ritorno nel launcher.", "Ativa os sons de navegação, confirmação e retrocesso no iniciador.","在启动器中启用导航、确认与返回音效。","在啟動器中啟用導覽、確認與返回音效。"},
    {"Checks the latest published ARMSX2-nx GitHub release in the background when the launcher opens. Turn it off to check only from the launcher settings screen.", "Recherche en arrière-plan la dernière version publiée de ARMSX2-nx sur GitHub à l'ouverture du lanceur. Désactivez cette option pour vérifier uniquement depuis les paramètres.", "Prüft beim Öffnen des Launchers im Hintergrund die neueste veröffentlichte ARMSX2-nx-Version auf GitHub. Ausschalten, um nur über die Launcher-Einstellungen zu prüfen.", "Busca en segundo plano la última versión publicada de ARMSX2-nx en GitHub al abrir el lanzador. Desactívalo para comprobar solo desde los ajustes.", "Controlla in background l'ultima versione pubblicata di ARMSX2-nx su GitHub all'apertura del launcher. Disattivare per controllare solo dalle impostazioni.", "Procura em segundo plano a versão mais recente do ARMSX2-nx publicada no GitHub ao abrir o iniciador. Desative para verificar apenas nas definições.","启动器打开时在后台检查最新发布的 ARMSX2-nx GitHub 版本。关闭后仅可从启动器设置界面检查。","啟動器開啟時在背景檢查最新發佈的 ARMSX2-nx GitHub 版本。關閉後僅可從啟動器設定畫面檢查。"},
    {"Selects the language used by the launcher. System follows the language configured on the Switch. Game names, paths, server messages and user-entered text are never translated.",
  "Sélectionne la langue du lanceur. Système suit la langue configurée sur la Switch. Les noms de jeux, chemins, messages serveur et textes saisis ne sont jamais traduits.",
  "Wählt die Sprache des Launchers. System folgt der auf der Switch eingestellten Sprache. Spielnamen, Pfade, Servermeldungen und Benutzereingaben werden nie übersetzt.",
  "Selecciona el idioma del lanzador. Sistema sigue el idioma configurado en Switch. Los nombres de juegos, rutas, mensajes del servidor y textos del usuario nunca se traducen.",
  "Seleziona la lingua del launcher. Sistema segue la lingua configurata su Switch. Nomi dei giochi, percorsi, messaggi del server e testo inserito non vengono mai tradotti.",
  "Seleciona o idioma do launcher. Sistema segue o idioma configurado na Switch. Nomes dos jogos, caminhos, mensagens do servidor e texto introduzido nunca são traduzidos.","选择启动器使用的语言。系统跟随 Switch 上配置的语言。游戏名称、路径、服务器消息与用户输入文本永不翻译。","選擇啟動器使用的語言。系統跟隨 Switch 上設定的語言。遊戲名稱、路徑、伺服器訊息與使用者輸入文字永不翻譯。"},
    {"Overrides the aspect ratio used for pre-rendered movie sequences independently of gameplay. Leave it Off unless a game's FMV is shown with the wrong shape.",
  "Remplace le format d'image des séquences vidéo pré-rendues indépendamment du jeu. Laissez désactivé sauf si les FMV d'un jeu ont une forme incorrecte.",
  "Überschreibt das Seitenverhältnis vorgerenderter Filmsequenzen unabhängig vom Spiel. Aus lassen, sofern die FMVs eines Spiels nicht falsch dargestellt werden.",
  "Cambia la relación de aspecto de los vídeos prerenderizados sin afectar al juego. Déjalo desactivado salvo que los FMV se muestren con forma incorrecta.",
  "Sostituisce il rapporto d'aspetto dei filmati prerenderizzati indipendentemente dal gioco. Lascia disattivato salvo FMV dalla forma errata.",
  "Substitui a proporção dos vídeos pré-renderizados sem afetar o jogo. Deixe desligado salvo se os FMV forem apresentados com formato errado.","独立于游戏画面覆盖预渲染影片序列的宽高比。除非某游戏 FMV 显示比例错误，否则保持关闭。","獨立於遊戲畫面覆寫預渲染影片序列的寬高比。除非某遊戲 FMV 顯示比例錯誤，否則保持關閉。"},
    {"Selects where bilinear filtering is used on PS2 textures. PS2 follows each game's request; Forced smooths more surfaces, while Nearest keeps hard pixel edges. Forced filtering can blur 2D sprites.",
  "Sélectionne où appliquer le filtrage bilinéaire aux textures PS2. PS2 suit la demande du jeu ; Forcé lisse plus de surfaces, tandis que Plus proche garde les pixels nets. Le filtrage forcé peut flouter les sprites 2D.",
  "Wählt, wo bilineare Filterung auf PS2-Texturen angewendet wird. PS2 folgt dem Spiel; Erzwungen glättet mehr Flächen, Nächster erhält harte Pixelkanten. Erzwungene Filterung kann 2D-Sprites verwischen.",
  "Selecciona dónde usar filtrado bilineal en texturas PS2. PS2 sigue lo pedido por cada juego; Forzado suaviza más superficies y Cercano conserva bordes duros. El filtrado forzado puede desenfocar sprites 2D.",
  "Seleziona dove usare il filtro bilineare sulle texture PS2. PS2 segue la richiesta del gioco; Forzato leviga più superfici, Vicino mantiene bordi netti. Il filtro forzato può sfocare gli sprite 2D.",
  "Seleciona onde usar filtragem bilinear nas texturas PS2. PS2 segue o pedido do jogo; Forçado suaviza mais superfícies e Mais próximo mantém píxeis nítidos. A filtragem forçada pode desfocar sprites 2D.","选择在 PS2 贴图上使用双线性过滤的位置。PS2 跟随各游戏请求；强制可平滑更多表面，最近邻则保持硬像素边缘。强制过滤可能模糊 2D 精灵。","選擇在 PS2 貼圖上使用雙線性過濾的位置。PS2 跟隨各遊戲請求；強制可平滑更多表面，最近鄰則保持硬像素邊緣。強制過濾可能模糊 2D 精靈。"},
    {"Improves texture clarity at steep viewing angles. Higher values add some GPU work and are most useful when rendering above native resolution.",
  "Améliore la netteté des textures vues sous un angle prononcé. Les valeurs élevées ajoutent du travail GPU et sont surtout utiles au-dessus de la résolution native.",
  "Verbessert die Texturschärfe bei flachen Blickwinkeln. Höhere Werte belasten die GPU etwas und sind vor allem oberhalb der nativen Auflösung nützlich.",
  "Mejora la nitidez de las texturas en ángulos pronunciados. Los valores altos añaden trabajo de GPU y son más útiles por encima de la resolución nativa.",
  "Migliora la nitidezza delle texture ad angoli estremi. Valori maggiori aggiungono lavoro GPU e servono soprattutto oltre la risoluzione nativa.",
  "Melhora a nitidez das texturas em ângulos acentuados. Valores maiores acrescentam trabalho à GPU e são mais úteis acima da resolução nativa.","改善陡峭视角下的贴图清晰度。更高数值增加一定 GPU 工作，在高于原生分辨率渲染时最有用。","改善陡峭視角下的貼圖清晰度。更高數值增加一定 GPU 工作，在高於原生解析度渲染時最有用。"},
    {"Achievements will activate the next time a game starts.","Les succès s'activeront au prochain démarrage d'un jeu.","Erfolge werden beim nächsten Spielstart aktiviert.","Los logros se activarán la próxima vez que inicies un juego.","Gli obiettivi si attiveranno al prossimo avvio di un gioco.","As conquistas serão ativadas no próximo arranque de um jogo.","成就将在下次启动游戏时激活。","成就將在下次啟動遊戲時啟用。"},
    {"Artwork search failed","Échec de la recherche d'images","Bildsuche fehlgeschlagen","Falló la búsqueda de imágenes","Ricerca immagini non riuscita","Falha na pesquisa de imagens","封面搜索失败","封面搜尋失敗"},
    {"Choose another destination or rename the folder first.","Choisissez une autre destination ou renommez d'abord le dossier.","Wählen Sie ein anderes Ziel oder benennen Sie zuerst den Ordner um.","Elige otro destino o cambia primero el nombre de la carpeta.","Scegli un'altra destinazione o rinomina prima la cartella.","Escolha outro destino ou mude primeiro o nome da pasta.","请选择其他目标或先重命名文件夹。","請選擇其他目標或先重新命名資料夾。"},
    {"Copy a PS2 BIOS dump into:","Copiez un dump du BIOS PS2 dans :","Kopieren Sie einen PS2-BIOS-Dump nach:","Copia un volcado de la BIOS de PS2 en:","Copia un dump del BIOS PS2 in:","Copie um dump da BIOS da PS2 para:","将 PS2 BIOS 转储复制到：","將 PS2 BIOS 傾印複製到："},
    {"Cover search failed","Échec de la recherche de jaquette","Cover-Suche fehlgeschlagen","Falló la búsqueda de carátula","Ricerca copertina non riuscita","Falha na pesquisa de capa","封面搜索失败","封面搜尋失敗"},
    {"Delete failed","Échec de la suppression","Löschen fehlgeschlagen","Falló la eliminación","Eliminazione non riuscita","Falha ao eliminar","删除失败","刪除失敗"},
    {"Delete game?","Supprimer le jeu ?","Spiel löschen?","¿Eliminar el juego?","Eliminare il gioco?","Eliminar o jogo?","删除游戏？","刪除遊戲？"},
    {"Folder already exists","Le dossier existe déjà","Ordner ist bereits vorhanden","La carpeta ya existe","La cartella esiste già","A pasta já existe","文件夹已存在","資料夾已存在"},
    {"Folder unavailable","Dossier indisponible","Ordner nicht verfügbar","Carpeta no disponible","Cartella non disponibile","Pasta indisponível","文件夹不可用","資料夾無法使用"},
    {"Games will not boot until you add one.","Les jeux ne démarreront pas tant que vous n'en aurez pas ajouté un.","Spiele starten erst, nachdem Sie eines hinzugefügt haben.","Los juegos no se iniciarán hasta que añadas una.","I giochi non si avvieranno finché non ne aggiungi uno.","Os jogos não arrancarão até adicionar uma.","添加 BIOS 前游戏无法启动。","新增 BIOS 前遊戲無法啟動。"},
    {"its storage device. This cannot be undone.","son périphérique de stockage. Cette action est irréversible.","seinem Speichergerät. Dies kann nicht rückgängig gemacht werden.","su dispositivo de almacenamiento. Esta acción no se puede deshacer.","dal relativo dispositivo di archiviazione. L'operazione è irreversibile.","do respetivo dispositivo de armazenamento. Esta ação não pode ser anulada.","存储设备。此操作无法撤销。","儲存裝置。此操作無法復原。"},
    {"No files will be deleted.","Aucun fichier ne sera supprimé.","Es werden keine Dateien gelöscht.","No se eliminará ningún archivo.","Nessun file verrà eliminato.","Nenhum ficheiro será eliminado.","不会删除任何文件。","不會刪除任何檔案。"},
    {"No PS2 BIOS found","Aucun BIOS PS2 trouvé","Kein PS2-BIOS gefunden","No se encontró una BIOS de PS2","Nessun BIOS PS2 trovato","Nenhuma BIOS da PS2 encontrada","未找到 PS2 BIOS","找不到 PS2 BIOS"},
    {"Remove game folder?","Retirer le dossier de jeux ?","Spieleordner entfernen?","¿Quitar la carpeta de juegos?","Rimuovere la cartella dei giochi?","Remover a pasta de jogos?","移除游戏文件夹？","移除遊戲資料夾？"},
    {"Rename failed","Échec du renommage","Umbenennen fehlgeschlagen","Falló el cambio de nombre","Rinomina non riuscita","Falha ao mudar o nome","重命名失败","重新命名失敗"},
    {"Replace existing file?","Remplacer le fichier existant ?","Vorhandene Datei ersetzen?","¿Sustituir el archivo existente?","Sostituire il file esistente?","Substituir o ficheiro existente?","替换已有文件？","取代既有檔案？"},
    {"RetroAchievements","RetroAchievements","RetroAchievements","RetroAchievements","RetroAchievements","RetroAchievements","Retro成就","Retro成就"},
    {"RetroAchievements sign-in failed","Échec de la connexion à RetroAchievements","RetroAchievements-Anmeldung fehlgeschlagen","Falló el inicio de sesión en RetroAchievements","Accesso a RetroAchievements non riuscito","Falha ao iniciar sessão no RetroAchievements","Retro成就 登录失败","Retro成就 登入失敗"},
    {"Signed in as ","Connecté en tant que ","Angemeldet als ","Sesión iniciada como ","Accesso effettuato come ","Sessão iniciada como ","已登录为 ","已登入為 "},
    {"The device may be disconnected.","Le périphérique est peut-être déconnecté.","Das Gerät wurde möglicherweise getrennt.","Es posible que el dispositivo esté desconectado.","Il dispositivo potrebbe essere scollegato.","O dispositivo pode estar desligado.","设备可能已断开。","裝置可能已中斷連線。"},
    {"The existing file will be replaced.","Le fichier existant sera remplacé.","Die vorhandene Datei wird ersetzt.","Se sustituirá el archivo existente.","Il file esistente verrà sostituito.","O ficheiro existente será substituído.","已有文件将被替换。","既有檔案將被取代。"},
    {"The file transfer could not be completed.","Le transfert du fichier n'a pas pu être terminé.","Die Dateiübertragung konnte nicht abgeschlossen werden.","No se pudo completar la transferencia del archivo.","Impossibile completare il trasferimento del file.","Não foi possível concluir a transferência do ficheiro.","文件传输未能完成。","檔案傳輸未能完成。"},
    {"The installed launcher was left unchanged.","Le lanceur installé n'a pas été modifié.","Der installierte Launcher wurde nicht verändert.","El lanzador instalado no se ha modificado.","Il launcher installato non è stato modificato.","O launcher instalado não foi alterado.","已安装的启动器未更改。","已安裝的啟動器未變更。"},
    {"This permanently deletes the game file from","Cette action supprime définitivement le fichier du jeu de","Dadurch wird die Spieldatei dauerhaft gelöscht von","Esto elimina permanentemente el archivo del juego de","Elimina definitivamente il file del gioco da","Isto elimina permanentemente o ficheiro do jogo de","这将永久删除游戏文件，来自","這將永久刪除遊戲檔案，來自"},
    {"Transfer failed","Échec du transfert","Übertragung fehlgeschlagen","Falló la transferencia","Trasferimento non riuscito","Falha na transferência","传输失败","傳輸失敗"},
    {"Update recovery failed","Échec de la récupération de la mise à jour","Update-Wiederherstellung fehlgeschlagen","Falló la recuperación de la actualización","Ripristino dell'aggiornamento non riuscito","Falha na recuperação da atualização","更新恢复失败","更新復原失敗"},
    {"General", "Général", "Allgemein", "General", "Generale", "Geral","常规","一般"},
    {"Emulator", "Émulateur", "Emulator", "Emulador", "Emulatore", "Emulador","模拟器","模擬器"},
    {"Manage game", "Gestion du jeu", "Spiel verwalten", "Gestionar juego", "Gestisci gioco", "Gerir jogo","管理游戏","管理遊戲"},
    {"Game menu", "Menu du jeu", "Spielmenü", "Menú del juego", "Menu gioco", "Menu do jogo","游戏菜单","遊戲選單"},
    {"Preview unavailable", "Aperçu indisponible", "Vorschau nicht verfügbar", "Vista previa no disponible", "Anteprima non disponibile", "Pré-visualização indisponível","预览不可用","無法預覽"},
    {"Use artwork", "Utiliser l'illustration", "Bild verwenden", "Usar ilustración", "Usa immagine", "Usar imagem","使用封面","使用封面"},
    {"Select", "Sélectionner", "Auswählen", "Seleccionar", "Seleziona", "Selecionar","选择","選擇"},
    {"Not signed in", "Non connecté", "Nicht angemeldet", "Sin sesión iniciada", "Non connesso", "Sem sessão iniciada","未登录","未登入"},
    {"Locations", "Emplacements", "Speicherorte", "Ubicaciones", "Percorsi", "Localizações","位置","位置"},
    {"Rename", "Renommer", "Umbenennen", "Renombrar", "Rinomina", "Renomear","重命名","重新命名"},
    {"Rename game", "Renommer le jeu", "Spiel umbenennen", "Renombrar juego", "Rinomina gioco", "Renomear jogo","重命名游戏","重新命名遊戲"},
    {"Clear game settings", "Effacer les paramètres du jeu", "Spieleinstellungen löschen", "Borrar ajustes del juego", "Cancella impostazioni gioco", "Limpar definições do jogo","清除游戏设置","清除遊戲設定"},
    {"Delete game", "Supprimer le jeu", "Spiel löschen", "Eliminar juego", "Elimina gioco", "Eliminar jogo","删除游戏","刪除遊戲"},
    {"Account", "Compte", "Konto", "Cuenta", "Account", "Conta","账户","帳戶"},
    {"Achievements", "Succès", "Erfolge", "Logros", "Obiettivi", "Conquistas","成就","成就"},
    {"Notifications", "Notifications", "Benachrichtigungen", "Notificaciones", "Notifiche", "Notificações","通知","通知"},
    {"Change account", "Changer de compte", "Konto wechseln", "Cambiar de cuenta", "Cambia account", "Mudar de conta","更换账户","更換帳戶"},
    {"Sign in", "Se connecter", "Anmelden", "Iniciar sesión", "Accedi", "Iniciar sessão","登录","登入"},
    {"Sign out", "Se déconnecter", "Abmelden", "Cerrar sesión", "Esci", "Terminar sessão","退出登录","登出"},
    {"Select game folder", "Sélectionner un dossier de jeux", "Spieleordner auswählen", "Seleccionar carpeta de juegos", "Seleziona cartella giochi", "Selecionar pasta de jogos","选择游戏文件夹","選擇遊戲資料夾"},
    {"All folders are scanned by ARMSX2-nx", "Tous les dossiers sont analysés par ARMSX2-nx", "Alle Ordner werden von ARMSX2-nx durchsucht", "ARMSX2-nx analiza todas las carpetas", "Tutte le cartelle vengono analizzate da ARMSX2-nx", "Todas as pastas são analisadas pelo ARMSX2-nx","ARMSX2-nx 会扫描所有文件夹","ARMSX2-nx 會掃描所有資料夾"},
    {"File transfer", "Transfert de fichiers", "Dateiübertragung", "Transferencia de archivos", "Trasferimento file", "Transferência de ficheiros","文件传输","檔案傳輸"},
    {"Logging", "Journalisation", "Protokollierung", "Registro", "Registro", "Registo", "日志", "日誌"},
    {"Controllers", "Manettes", "Controller", "Mandos", "Controller", "Comandos", "手柄", "手把"},
    {"Controller 1", "Manette 1", "Controller 1", "Mando 1", "Controller 1", "Comando 1", "手柄 1", "手把 1"},
    {"Controller 2", "Manette 2", "Controller 2", "Mando 2", "Controller 2", "Comando 2", "手柄 2", "手把 2"},
    {"BIOS and boot", "BIOS et démarrage", "BIOS und Start", "BIOS y arranque", "BIOS e avvio", "BIOS e arranque", "BIOS 与启动", "BIOS 與啟動"},
    {"Network and HDD", "Réseau et disque dur", "Netzwerk und Festplatte", "Red y disco duro", "Rete e disco rigido", "Rede e disco rígido", "网络与硬盘", "網路與硬碟"},
    {"Achievement options", "Options des succès", "Erfolgsoptionen", "Opciones de logros", "Opzioni obiettivi", "Opções de conquistas", "成就选项", "成就選項"},
    {"Frame generation", "Génération d'images", "Frame-Generierung", "Generación de fotogramas", "Generazione fotogrammi", "Geração de fotogramas", "帧生成", "幀生成"},
    {"Flow resolution", "Résolution du flux", "Flussauflösung", "Resolución del flujo", "Risoluzione del flusso", "Resolução do fluxo", "光流分辨率", "光流解析度"},
    {"Quarter resolution", "Quart de résolution", "Viertelauflösung", "Cuarto de resolución", "Quarto di risoluzione", "Quarto da resolução", "四分之一分辨率", "四分之一解析度"},
    {"Half resolution", "Demi-résolution", "Halbe Auflösung", "Media resolución", "Metà risoluzione", "Meia resolução", "二分之一分辨率", "二分之一解析度"},
    {"Full resolution", "Pleine résolution", "Volle Auflösung", "Resolución completa", "Risoluzione piena", "Resolução completa", "全分辨率", "全解析度"},
    {"Performance mode", "Mode performances", "Leistungsmodus", "Modo rendimiento", "Modalità prestazioni", "Modo de desempenho", "性能模式", "效能模式"},
    {"120 Hz Docked Mode", "Mode station 120 Hz", "120-Hz-Dockmodus", "Modo base a 120 Hz", "Modalità dock a 120 Hz", "Modo base a 120 Hz", "120 Hz 底座模式", "120 Hz 底座模式"},
    {"Installed", "Installé", "Installiert", "Instalado", "Installato", "Instalado", "已安装", "已安裝"},
    {"Missing", "Manquant", "Fehlt", "Ausente", "Mancante", "Em falta", "缺失", "遺失"},
    {"Required component", "Composant requis", "Erforderliche Komponente", "Componente requerido", "Componente richiesto", "Componente necessário", "必需组件", "必要元件"},
    {"Frame generation quality", "Qualité de génération d'images", "Qualität der Frame-Generierung", "Calidad de generación de fotogramas", "Qualità della generazione fotogrammi", "Qualidade da geração de fotogramas", "帧生成质量", "幀生成品質"},
    {"Frame generation performance", "Performances de génération d'images", "Leistung der Frame-Generierung", "Rendimiento de generación de fotogramas", "Prestazioni della generazione fotogrammi", "Desempenho da geração de fotogramas", "帧生成性能", "幀生成效能"},
    {"LSFG 2x enabled", "LSFG 2x activé", "LSFG 2x aktiviert", "LSFG 2x activado", "LSFG 2x attivo", "LSFG 2x ativado", "LSFG 2x 已启用", "LSFG 2x 已啟用"},
    {"Lossless.dll missing", "Lossless.dll manquant", "Lossless.dll fehlt", "Falta Lossless.dll", "Lossless.dll mancante", "Lossless.dll em falta", "缺少 Lossless.dll", "缺少 Lossless.dll"},
    {"LSFG is not ready", "LSFG n'est pas prêt", "LSFG ist nicht bereit", "LSFG no está listo", "LSFG non è pronto", "O LSFG não está pronto", "LSFG 未就绪", "LSFG 未就緒"},
    {"Lossless.dll was not found.", "Lossless.dll est introuvable.", "Lossless.dll wurde nicht gefunden.", "No se encontró Lossless.dll.", "Lossless.dll non trovato.", "Lossless.dll não foi encontrado.", "未找到 Lossless.dll。", "未找到 Lossless.dll。"},
    {"Copy it to:", "Copiez-le dans :", "Kopieren Sie sie nach:", "Cópialo en:", "Copialo in:", "Copie-o para:", "请将其复制到：", "請將其複製到："},
    {"then enable LSFG again.", "puis réactivez LSFG.", "und aktivieren Sie LSFG dann erneut.", "y vuelve a activar LSFG.", "quindi riattiva LSFG.", "e depois ative o LSFG novamente.", "然后再次启用 LSFG。", "然後再次啟用 LSFG。"},
    {"LSFG disabled for this launch", "LSFG désactivé pour ce lancement", "LSFG für diesen Start deaktiviert", "LSFG desactivado para este inicio", "LSFG disattivato per questo avvio", "LSFG desativado neste arranque", "本次启动已禁用 LSFG", "本次啟動已停用 LSFG"},
    {"GS multithreading", "Multithreading GS", "GS-Multithreading", "Multihilo del GS", "Multithreading GS", "Multithreading do GS", "GS 多线程", "GS 多執行緒"},
    {"Renderer", "Moteur de rendu", "Renderer", "Renderizador", "Renderer", "Renderizador", "渲染器", "渲染器"},
    {"Hardcore mode", "Mode hardcore", "Hardcore-Modus", "Modo hardcore", "Modalità hardcore", "Modo hardcore", "硬核模式", "硬核模式"},
    {"SD card", "Carte SD", "SD-Karte", "Tarjeta SD", "Scheda SD", "Cartão SD", "SD 卡", "SD 卡"},
    {"games / files / covers", "jeux / fichiers / jaquettes", "Spiele / Dateien / Cover", "juegos / archivos / portadas", "giochi / file / copertine", "jogos / ficheiros / capas", "游戏 / 文件 / 封面", "遊戲 / 檔案 / 封面"},
    {"Preparing the emulator", "Préparation de l'émulateur", "Emulator wird vorbereitet", "Preparando el emulador", "Preparazione dell'emulatore", "A preparar o emulador", "正在准备模拟器", "正在準備模擬器"},
    {"Drako3D core", "Cœur Drako3D", "Drako3D-Kern", "Núcleo Drako3D", "Core Drako3D", "Núcleo Drako3D", "Drako3D 核心", "Drako3D 核心"},
    {"Vulkan core", "Cœur Vulkan", "Vulkan-Kern", "Núcleo Vulkan", "Core Vulkan", "Núcleo Vulkan", "Vulkan 核心", "Vulkan 核心"},
    {"OpenGL core", "Cœur OpenGL", "OpenGL-Kern", "Núcleo OpenGL", "Core OpenGL", "Núcleo OpenGL", "OpenGL 核心", "OpenGL 核心"},
};

std::string SystemCode()
{
  if (R_FAILED(setInitialize()))
    return "en";
  u64 code = 0;
  SetLanguage language = SetLanguage_ENUS;
  const bool ok = R_SUCCEEDED(setGetSystemLanguage(&code)) &&
                  R_SUCCEEDED(setMakeLanguage(code, &language));
  setExit();
  if (!ok)
    return "en";
  switch (language)
  {
  case SetLanguage_FR:
  case SetLanguage_FRCA:
    return "fr";
  case SetLanguage_DE:
    return "de";
  case SetLanguage_ES:
  case SetLanguage_ES419:
    return "es";
  case SetLanguage_IT:
    return "it";
  case SetLanguage_PT:
  case SetLanguage_PTBR:
    return "pt";
  case SetLanguage_ZHCN:
  case SetLanguage_ZHHANS:
    return "zh-Hans";
  case SetLanguage_ZHTW:
  case SetLanguage_ZHHANT:
    return "zh-Hant";
  default:
    return "en";
  }
}
}  // namespace

const std::vector<LauncherLanguage>& LauncherLocalization::Languages()
{
  static const std::vector<LauncherLanguage> languages(s_languages.begin(), s_languages.end());
  return languages;
}

int LauncherLocalization::FindLanguage(std::string_view code)
{
  const auto& languages = Languages();
  const auto found = std::find_if(languages.begin(), languages.end(),
                                  [&](const auto& item) { return code == item.code; });
  return found == languages.end() ? 0 : static_cast<int>(found - languages.begin());
}

bool LauncherLocalization::SetLanguage(std::string_view preference)
{
  const int selected = FindLanguage(preference);
  m_preference = Languages()[selected].code;
  m_resolved = m_preference == "system" ? SystemCode() : m_preference;
  m_catalog.clear();
  int column = 0;
  if (m_resolved == "fr") column = 1;
  else if (m_resolved == "de") column = 2;
  else if (m_resolved == "es") column = 3;
  else if (m_resolved == "it") column = 4;
  else if (m_resolved == "pt") column = 5;
  else if (m_resolved == "zh-Hans") column = 6;
  else if (m_resolved == "zh-Hant") column = 7;
  if (column)
  {
    for (const Translation& item : s_text)
    {
      const char* values[] = {
          item.en, item.fr, item.de, item.es, item.it, item.pt, item.zhHans, item.zhHant};
      if (values[column] && values[column][0])
        m_catalog.emplace(item.en, values[column]);
    }
  }
  return true;
}

std::string_view LauncherLocalization::Translate(std::string_view source) const
{
  const auto found = m_catalog.find(std::string(source));
  return found == m_catalog.end() ? source : std::string_view(found->second);
}

const char* LauncherLocalization::DisplayName() const
{
  const auto& languages = Languages();
  const int index = FindLanguage(m_preference);
  return languages[index].name;
}
