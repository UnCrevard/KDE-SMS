#include "i18n.h"
#include <unordered_map>
#include <cwctype>

namespace KDEConnect {

    namespace {

        struct LangEntry {
            const char* key;
            const wchar_t* fr;
            const wchar_t* en;
            const wchar_t* es;
            const wchar_t* de;
            const wchar_t* it;
            const wchar_t* pt;
        };

        const LangEntry kStrings[] = {
            {
                "app_title_base",
                L"KDE Connect SMS - Windows Win32",
                L"KDE Connect SMS - Windows Win32",
                L"KDE Connect SMS - Windows Win32",
                L"KDE Connect SMS - Windows Win32",
                L"KDE Connect SMS - Windows Win32",
                L"KDE Connect SMS - Windows Win32"
            },
            {
                "title_net_error",
                L"[Erreur r\u00e9seau]",
                L"[Network error]",
                L"[Error de red]",
                L"[Netzwerkfehler]",
                L"[Errore di rete]",
                L"[Erro de rede]"
            },
            {
                "search_placeholder",
                L"Rechercher un contact ou SMS...",
                L"Search a contact or SMS...",
                L"Buscar un contacto o SMS...",
                L"Kontakt oder SMS suchen...",
                L"Cerca un contatto o SMS...",
                L"Pesquisar contato ou SMS..."
            },
            {
                "btn_send",
                L"Envoyer",
                L"Send",
                L"Enviar",
                L"Senden",
                L"Invia",
                L"Enviar"
            },
            {
                "btn_settings",
                L"\u2699 R\u00e9glages",
                L"\u2699 Settings",
                L"\u2699 Ajustes",
                L"\u2699 Einstellungen",
                L"\u2699 Impostazioni",
                L"\u2699 Defini\u00e7\u00f5es"
            },
            {
                "pending_attachment_label",
                L"\U0001F4CE Pi\u00e8ce jointe pr\u00eate \u00e0 envoyer : {0}",
                L"\U0001F4CE Attachment ready to send: {0}",
                L"\U0001F4CE Archivo adjunto listo para enviar: {0}",
                L"\U0001F4CE Anhang bereit zum Senden: {0}",
                L"\U0001F4CE Allegato pronto per l'invio: {0}",
                L"\U0001F4CE Anexo pronto para enviar: {0}"
            },
            {
                "empty_state_msg",
                L"Aucune conversation SMS charg\u00e9e.\n\nStatut : {0}\n\nAssurez-vous que KDE Connect est appair\u00e9 sur le t\u00e9l\u00e9phone,\nque l'autorisation SMS est accord\u00e9e, puis cliquez sur \u21bb.",
                L"No SMS conversation loaded.\n\nStatus: {0}\n\nMake sure KDE Connect is paired on your phone,\nSMS permission is granted, then click \u21bb.",
                L"No hay conversaciones SMS cargadas.\n\nEstado: {0}\n\nAseg\u00farese de que KDE Connect est\u00e9 emparejado en el tel\u00e9fono,\ncon permiso SMS concedido, y haga clic en \u21bb.",
                L"Keine SMS-Konversation geladen.\n\nStatus: {0}\n\nStellen Sie sicher, dass KDE Connect auf dem Telefon gekoppelt ist,\ndie SMS-Berechtigung erteilt wurde, und klicken Sie auf \u21bb.",
                L"Nessuna conversazione SMS caricata.\n\nStato: {0}\n\nAssicurati che KDE Connect sia associato sul telefono,\nche l'autorizzazione SMS sia concessa, poi clicca su \u21bb.",
                L"Nenhuma conversa SMS carregada.\n\nEstado: {0}\n\nCertifique-se de que o KDE Connect est\u00e1 emparelhado no telefone,\nque a permiss\u00e3o SMS est\u00e1 ativa e clique em \u21bb."
            },
            {
                "contact_prefix",
                L"Contact \u00b7 {0}",
                L"Contact \u00b7 {0}",
                L"Contacto \u00b7 {0}",
                L"Kontakt \u00b7 {0}",
                L"Contatto \u00b7 {0}",
                L"Contato \u00b7 {0}"
            },
            {
                "delete_confirm_prompt",
                L"Effacer la conversation avec \u00ab {0} \u00bb de la liste ?",
                L"Delete the conversation with \"{0}\" from the list?",
                L"\u00bfEliminar la conversaci\u00f3n con \"{0}\" de la lista?",
                L"Konversation mit \"{0}\" aus der Liste l\u00f6schen?",
                L"Eliminare la conversazione con \"{0}\" dall'elenco?",
                L"Apagar a conversa com \"{0}\" da lista?"
            },
            {
                "delete_confirm_title",
                L"Supprimer la conversation",
                L"Delete conversation",
                L"Eliminar conversaci\u00f3n",
                L"Konversation l\u00f6schen",
                L"Elimina conversazione",
                L"Apagar conversa"
            },
            {
                "menu_delete_conv",
                L"Supprimer la conversation",
                L"Delete conversation",
                L"Eliminar conversaci\u00f3n",
                L"Konversation l\u00f6schen",
                L"Elimina conversazione",
                L"Apagar conversa"
            },
            {
                "send_error_msg",
                L"Impossible d'envoyer le message ou la pi\u00e8ce jointe.\nV\u00e9rifiez que le t\u00e9l\u00e9phone est connect\u00e9 et appair\u00e9, et que le fichier est accessible.",
                L"Unable to send the message or attachment.\nCheck that your phone is connected and paired, and that the file is accessible.",
                L"No se pudo enviar el mensaje o archivo adjunto.\nVerifique que el tel\u00e9fono est\u00e9 conectado y emparejado, y que el archivo sea accesible.",
                L"Nachricht oder Anhang konnte nicht gesendet werden.\nPr\u00fcfen Sie, ob Ihr Telefon verbunden und gekoppelt ist und die Datei zug\u00e4nglich ist.",
                L"Impossibile inviare il messaggio o l'allegato.\nVerifica che il telefono sia connesso e associato e che il file sia accessibile.",
                L"N\u00e3o foi poss\u00edvel enviar a mensagem ou o anexo.\nVerifique se o telefone est\u00e1 conectado e emparelhado e se o arquivo est\u00e1 acess\u00edvel."
            },
            {
                "send_error_title",
                L"\u00c9chec d'envoi",
                L"Send failed",
                L"Error al enviar",
                L"Senden fehlgeschlagen",
                L"Invio non riuscito",
                L"Falha ao enviar"
            },
            {
                "folder_picker_ok",
                L"Choisir ce dossier",
                L"Select this folder",
                L"Seleccionar esta carpeta",
                L"Diesen Ordner w\u00e4hlen",
                L"Scegli questa cartella",
                L"Selecionar esta pasta"
            },
            {
                "first_run_title",
                L"Premier d\u00e9marrage - Stockage des conversations",
                L"First launch - Conversation storage",
                L"Primer inicio - Almacenamiento de conversaciones",
                L"Erster Start - Speicherort f\u00fcr Konversationen",
                L"Primo avvio - Archiviazione conversazioni",
                L"Primeiro in\u00edcio - Armazenamento de conversas"
            },
            {
                "first_run_msg",
                L"Bienvenue dans KDE Connect SMS !\n\nPour ce premier d\u00e9marrage, choisissez le dossier o\u00f9 stocker vos conversations.\nLes r\u00e9glages de l'application seront enregistr\u00e9s dans :\n{0}\n\n\u2022 OK : Parcourir et choisir un dossier\n\u2022 Annuler : Utiliser le dossier par d\u00e9faut :\n  {1}",
                L"Welcome to KDE Connect SMS!\n\nOn first launch, choose the folder to store your conversations.\nApplication settings will be saved in:\n{0}\n\n\u2022 OK: Browse and choose a folder\n\u2022 Cancel: Use default folder:\n  {1}",
                L"\u00a1Bienvenido a KDE Connect SMS!\n\nEn este primer inicio, elija la carpeta donde guardar sus conversaciones.\nLos ajustes se guardar\u00e1n en:\n{0}\n\n\u2022 Aceptar: Examinar y elegir una carpeta\n\u2022 Cancelar: Usar la carpeta predeterminada:\n  {1}",
                L"Willkommen bei KDE Connect SMS!\n\nW\u00e4hlen Sie beim ersten Start den Ordner zum Speichern Ihrer Konversationen.\nDie Einstellungen werden gespeichert in:\n{0}\n\n\u2022 OK: Ordner durchsuchen und ausw\u00e4hlen\n\u2022 Abbrechen: Standardordner verwenden:\n  {1}",
                L"Benvenuto in KDE Connect SMS!\n\nAl primo avvio, scegli la cartella in cui salvare le tue conversazioni.\nLe impostazioni verranno salvate in:\n{0}\n\n\u2022 OK: Sfoglia e scegli una cartella\n\u2022 Annulla: Usa la cartella predefinita:\n  {1}",
                L"Bem-vindo ao KDE Connect SMS!\n\nNeste primeiro in\u00edcio, escolha a pasta para armazenar suas conversas.\nAs configura\u00e7\u00f5es ser\u00e3o salvas em:\n{0}\n\n\u2022 OK: Procurar e escolher uma pasta\n\u2022 Cancelar: Usar pasta padr\u00e3o:\n  {1}"
            },
            {
                "first_run_picker_title",
                L"Choisir le dossier de stockage des conversations",
                L"Choose conversation storage folder",
                L"Elegir carpeta de almacenamiento",
                L"Speicherordner f\u00fcr Konversationen w\u00e4hlen",
                L"Scegli la cartella delle conversazioni",
                L"Escolher pasta de armazenamento das conversas"
            },
            {
                "tray_open",
                L"Ouvrir KDE Connect SMS",
                L"Open KDE Connect SMS",
                L"Abrir KDE Connect SMS",
                L"KDE Connect SMS \u00f6ffnen",
                L"Apri KDE Connect SMS",
                L"Abrir KDE Connect SMS"
            },
            {
                "tray_hide",
                L"Masquer la fen\u00eatre",
                L"Hide window",
                L"Ocultar ventana",
                L"Fenster ausblenden",
                L"Nascondi finestra",
                L"Ocultar janela"
            },
            {
                "tray_settings",
                L"R\u00e9glages...",
                L"Settings...",
                L"Ajustes...",
                L"Einstellungen...",
                L"Impostazioni...",
                L"Defini\u00e7\u00f5es..."
            },
            {
                "tray_quit",
                L"Quitter",
                L"Exit",
                L"Salir",
                L"Beenden",
                L"Esci",
                L"Sair"
            },
            {
                "bind_alert_msg",
                L"Impossible d'ouvrir l'\u00e9coute KDE Connect sur l'adresse configur\u00e9e ({0}:1716) :\n\n{1}\n\nOuvrez \u00ab R\u00e9glages \u00bb pour choisir une interface r\u00e9seau valide ou fermez l'application qui utilise d\u00e9j\u00e0 le port 1716.",
                L"Unable to listen for KDE Connect on configured address ({0}:1716):\n\n{1}\n\nOpen \"Settings\" to pick a valid network interface or close the application already using port 1716.",
                L"No se pudo escuchar KDE Connect en la direcci\u00f3n configurada ({0}:1716):\n\n{1}\n\nAbra \"Ajustes\" para elegir una interfaz de red v\u00e1lida o cierre la aplicaci\u00f3n que ya usa el puerto 1716.",
                L"KDE Connect konnte auf der konfigurierten Adresse ({0}:1716) nicht lauschen:\n\n{1}\n\n\u00d6ffnen Sie \"Einstellungen\", um eine g\u00fcltige Netzwerkschnittstelle zu w\u00e4hlen, oder schlie\u00dfen Sie die App, die Port 1716 belegt.",
                L"Impossibile aprire l'ascolto KDE Connect sull'indirizzo configurato ({0}:1716):\n\n{1}\n\nApri \"Impostazioni\" per scegliere un'interfaccia di rete valida o chiudi l'applicazione che occupa la porta 1716.",
                L"N\u00e3o foi poss\u00edvel escutar o KDE Connect no endere\u00e7o configurado ({0}:1716):\n\n{1}\n\nAbra \"Defini\u00e7\u00f5es\" para escolher uma interface de rede v\u00e1lida ou feche o aplicativo que j\u00e1 usa a porta 1716."
            },
            {
                "bind_alert_title",
                L"Erreur de liaison r\u00e9seau",
                L"Network bind error",
                L"Error de enlace de red",
                L"Netzwerk-Bindungsfehler",
                L"Errore di binding di rete",
                L"Erro de associa\u00e7\u00e3o de rede"
            },
            {
                "ini_write_error_msg",
                L"Impossible d'enregistrer l'adresse BindIp dans :\n{0}\n\nV\u00e9rifiez que le dossier est accessible en \u00e9criture et que le fichier n'est pas verrouill\u00e9.",
                L"Failed to write BindIp address to:\n{0}\n\nCheck that the folder is writable and the file is not locked.",
                L"No se pudo guardar la direcci\u00f3n BindIp en:\n{0}\n\nVerifique que la carpeta tenga permisos de escritura y el archivo no est\u00e9 bloqueado.",
                L"BindIp-Adresse konnte nicht geschrieben werden in:\n{0}\n\nPr\u00fcfen Sie Schreibrechte und ob die Datei gesperrt ist.",
                L"Impossibile salvare l'indirizzo BindIp in:\n{0}\n\nVerifica che la cartella sia scrivibile e il file non sia bloccato.",
                L"N\u00e3o foi poss\u00edvel salvar o endere\u00e7o BindIp em:\n{0}\n\nVerifique se a pasta permite grava\u00e7\u00e3o e se o arquivo n\u00e3o est\u00e1 bloqueado."
            },
            {
                "ini_write_error_title",
                L"Erreur d'\u00e9criture INI",
                L"INI write error",
                L"Error de escritura INI",
                L"INI-Schreibfehler",
                L"Errore di scrittura INI",
                L"Erro de grava\u00e7\u00e3o INI"
            },
            {
                "settings_bind_failed_msg",
                L"Les r\u00e9glages ont \u00e9t\u00e9 enregistr\u00e9s dans :\n{0}\n\nCependant, la liaison r\u00e9seau sur l'adresse choisie a \u00e9chou\u00e9 :\n\n{1}",
                L"Settings were saved in:\n{0}\n\nHowever, binding on the selected network address failed:\n\n{1}",
                L"Los ajustes se guardaron en:\n{0}\n\nSin embargo, fall\u00f3 el enlace en la direcci\u00f3n de red elegida:\n\n{1}",
                L"Einstellungen wurden gespeichert in:\n{0}\n\nDie Netzwerkbindung an der gew\u00e4hlten Adresse ist jedoch fehlgeschlagen:\n\n{1}",
                L"Le impostazioni sono state salvate in:\n{0}\n\nTuttavia, il binding sull'indirizzo di rete selezionato \u00e8 fallito:\n\n{1}",
                L"As configura\u00e7\u00f5es foram salvas em:\n{0}\n\nNo entanto, a associa\u00e7\u00e3o no endere\u00e7o de rede escolhido falhou:\n\n{1}"
            },
            {
                "settings_bind_fallback_err",
                L"Impossible de binder UDP/TCP sur le port 1716.",
                L"Failed to bind UDP/TCP on port 1716.",
                L"No se pudo enlazar UDP/TCP en el puerto 1716.",
                L"UDP/TCP-Bindung auf Port 1716 fehlgeschlagen.",
                L"Impossibile associare UDP/TCP sulla porta 1716.",
                L"Falha ao associar UDP/TCP na porta 1716."
            },
            {
                "settings_title",
                L"R\u00e9glages - KDE Connect SMS",
                L"Settings - KDE Connect SMS",
                L"Ajustes - KDE Connect SMS",
                L"Einstellungen - KDE Connect SMS",
                L"Impostazioni - KDE Connect SMS",
                L"Defini\u00e7\u00f5es - KDE Connect SMS"
            },
            {
                "grp_storage",
                L"Dossiers d'enregistrement",
                L"Storage folders",
                L"Carpetas de almacenamiento",
                L"Speicherordner",
                L"Cartelle di archiviazione",
                L"Pastas de armazenamento"
            },
            {
                "lbl_conv_dir",
                L"Dossier des conversations (SMS, cache contacts et avatars) :",
                L"Conversations folder (SMS history, contact cache & avatars):",
                L"Carpeta de conversaciones (SMS, cach\u00e9 de contactos y avatares):",
                L"Konversationsordner (SMS-Verlauf, Kontaktcache & Avatare):",
                L"Cartella conversazioni (SMS, cache contatti e avatar):",
                L"Pasta de conversas (SMS, cache de contatos e avatares):"
            },
            {
                "lbl_recv_dir",
                L"Dossier des fichiers re\u00e7us (pi\u00e8ces jointes MMS) :",
                L"Received files folder (MMS attachments):",
                L"Carpeta de archivos recibidos (adjuntos MMS):",
                L"Ordner f\u00fcr empfangene Dateien (MMS-Anh\u00e4nge):",
                L"Cartella file ricevuti (allegati MMS):",
                L"Pasta de arquivos recebidos (anexos MMS):"
            },
            {
                "btn_browse",
                L"Parcourir...",
                L"Browse...",
                L"Examinar...",
                L"Durchsuchen...",
                L"Sfoglia...",
                L"Procurar..."
            },
            {
                "picker_conv_title",
                L"Choisir le dossier des conversations",
                L"Choose conversations folder",
                L"Elegir carpeta de conversaciones",
                L"Konversationsordner w\u00e4hlen",
                L"Scegli cartella conversazioni",
                L"Escolher pasta de conversas"
            },
            {
                "picker_recv_title",
                L"Choisir le dossier des fichiers re\u00e7us",
                L"Choose received files folder",
                L"Elegir carpeta de archivos recibidos",
                L"Ordner f\u00fcr empfangene Dateien w\u00e4hlen",
                L"Scegli cartella file ricevuti",
                L"Escolher pasta de arquivos recebidos"
            },
            {
                "grp_network",
                L"R\u00e9seau KDE Connect",
                L"KDE Connect Network",
                L"Red KDE Connect",
                L"KDE Connect Netzwerk",
                L"Rete KDE Connect",
                L"Rede KDE Connect"
            },
            {
                "lbl_bind_ip",
                L"Interface r\u00e9seau \u00e0 utiliser (Bind IPv4) :",
                L"Network interface to bind (IPv4):",
                L"Interfaz de red a usar (Bind IPv4):",
                L"Zu verwendende Netzwerkschnittstelle (IPv4):",
                L"Interfaccia di rete da usare (Bind IPv4):",
                L"Interface de rede a usar (Bind IPv4):"
            },
            {
                "bind_all_interfaces",
                L"Toutes les interfaces (0.0.0.0 - automatique)",
                L"All interfaces (0.0.0.0 - automatic)",
                L"Todas las interfaces (0.0.0.0 - autom\u00e1tico)",
                L"Alle Schnittstellen (0.0.0.0 - automatisch)",
                L"Tutte le interfacce (0.0.0.0 - automatico)",
                L"Todas as interfaces (0.0.0.0 - autom\u00e1tico)"
            },
            {
                "bind_custom_unavail",
                L"{0} (configur\u00e9e - non d\u00e9tect\u00e9e)",
                L"{0} (configured - not detected)",
                L"{0} (configurada - no detectada)",
                L"{0} (konfiguriert - nicht gefunden)",
                L"{0} (configurato - non rilevato)",
                L"{0} (configurado - n\u00e3o detectado)"
            },
            {
                "bind_status_err",
                L"INI : {0} | Erreur : {1}",
                L"INI: {0} | Error: {1}",
                L"INI: {0} | Error: {1}",
                L"INI: {0} | Fehler: {1}",
                L"INI: {0} | Errore: {1}",
                L"INI: {0} | Erro: {1}"
            },
            {
                "bind_status_ok",
                L"\u00c9coute active : {0}  |  INI BindIp : {1}",
                L"Active listener: {0}  |  INI BindIp: {1}",
                L"Escucha activa: {0}  |  INI BindIp: {1}",
                L"Aktiver Listener: {0}  |  INI BindIp: {1}",
                L"Ascolto attivo: {0}  |  INI BindIp: {1}",
                L"Escuta ativa: {0}  |  INI BindIp: {1}"
            },
            {
                "status_inactive",
                L"inactive",
                L"inactive",
                L"inactiva",
                L"inaktiv",
                L"inattivo",
                L"inativa"
            },
            {
                "lbl_device_name",
                L"Nom de cet ordinateur sur KDE Connect :",
                L"Device name shown on KDE Connect:",
                L"Nombre de este equipo en KDE Connect:",
                L"Ger\u00e4tename in KDE Connect:",
                L"Nome di questo computer su KDE Connect:",
                L"Nome deste computador no KDE Connect:"
            },
            {
                "grp_app_prefs",
                L"R\u00e9glages de l'application",
                L"Application preferences",
                L"Preferencias de la aplicaci\u00f3n",
                L"Anwendungseinstellungen",
                L"Preferenze dell'applicazione",
                L"Prefer\u00eancias do aplicativo"
            },
            {
                "lbl_theme",
                L"Th\u00e8me de l'interface :",
                L"Interface theme:",
                L"Tema de la interfaz:",
                L"Design der Oberfl\u00e4che:",
                L"Tema dell'interfaccia:",
                L"Tema da interface:"
            },
            {
                "theme_system",
                L"Syst\u00e8me (suivre Windows)",
                L"System (follow Windows)",
                L"Sistema (seguir a Windows)",
                L"System (Windows folgen)",
                L"Sistema (segui Windows)",
                L"Sistema (seguir o Windows)"
            },
            {
                "theme_light",
                L"Clair",
                L"Light",
                L"Claro",
                L"Hell",
                L"Chiaro",
                L"Claro"
            },
            {
                "theme_dark",
                L"Sombre",
                L"Dark",
                L"Oscuro",
                L"Dunkel",
                L"Scuro",
                L"Escuro"
            },
            {
                "lbl_language",
                L"Langue de l'interface :",
                L"Interface language:",
                L"Idioma de la interfaz:",
                L"Sprache der Oberfl\u00e4che:",
                L"Lingua dell'interfaccia:",
                L"Idioma da interface:"
            },
            {
                "lang_auto",
                L"Automatique (Langue du syst\u00e8me)",
                L"Automatic (System language)",
                L"Autom\u00e1tico (Idioma del sistema)",
                L"Automatisch (Systemsprache)",
                L"Automatico (Lingua di sistema)",
                L"Autom\u00e1tico (Idioma do sistema)"
            },
            {
                "lbl_sync",
                L"Synchronisation auto :",
                L"Auto sync:",
                L"Sincronizaci\u00f3n autom\u00e1tica:",
                L"Auto-Synchronisierung:",
                L"Sincronizzazione automatica:",
                L"Sincroniza\u00e7\u00e3o autom\u00e1tica:"
            },
            {
                "sync_3s",
                L"Toutes les 3 secondes",
                L"Every 3 seconds",
                L"Cada 3 segundos",
                L"Alle 3 Sekunden",
                L"Ogni 3 secondi",
                L"A cada 3 segundos"
            },
            {
                "sync_5s",
                L"Toutes les 5 secondes (par d\u00e9faut)",
                L"Every 5 seconds (default)",
                L"Cada 5 segundos (predeterminado)",
                L"Alle 5 Sekunden (Standard)",
                L"Ogni 5 secondi (predefinito)",
                L"A cada 5 segundos (padr\u00e3o)"
            },
            {
                "sync_10s",
                L"Toutes les 10 secondes",
                L"Every 10 seconds",
                L"Cada 10 segundos",
                L"Alle 10 Sekunden",
                L"Ogni 10 secondi",
                L"A cada 10 segundos"
            },
            {
                "sync_30s",
                L"Toutes les 30 secondes",
                L"Every 30 seconds",
                L"Cada 30 segundos",
                L"Alle 30 Sekunden",
                L"Ogni 30 secondi",
                L"A cada 30 segundos"
            },
            {
                "sync_60s",
                L"Toutes les 60 secondes",
                L"Every 60 seconds",
                L"Cada 60 segundos",
                L"Alle 60 Sekunden",
                L"Ogni 60 secondi",
                L"A cada 60 segundos"
            },
            {
                "sync_manual",
                L"Manuelle uniquement (d\u00e9sactiv\u00e9e)",
                L"Manual only (disabled)",
                L"Solo manual (desactivada)",
                L"Nur manuell (deaktiviert)",
                L"Solo manuale (disattivata)",
                L"Somente manual (desativada)"
            },
            {
                "chk_min_tray",
                L"R\u00e9duire dans la zone de notification lors de la fermeture",
                L"Minimize to system tray when closing",
                L"Minimizar a la bandeja del sistema al cerrar",
                L"Beim Schlie\u00dfen in den Infobereich minimieren",
                L"Riduci nell'area di notifica alla chiusura",
                L"Minimizar para a bandeja do sistema ao fechar"
            },
            {
                "chk_notifs",
                L"Activer les notifications bureau pour les nouveaux SMS",
                L"Enable desktop notifications for new SMS",
                L"Activar notificaciones de escritorio para nuevos SMS",
                L"Desktop-Benachrichtigungen f\u00fcr neue SMS aktivieren",
                L"Abilita notifiche desktop per i nuovi SMS",
                L"Ativar notifica\u00e7\u00f5es na \u00e1rea de trabalho para novos SMS"
            },
            {
                "chk_start_hidden",
                L"D\u00e9marrer l'application r\u00e9duite dans la zone de notification",
                L"Start application minimized to system tray",
                L"Iniciar aplicaci\u00f3n minimizada en la bandeja",
                L"Anwendung minimiert im Infobereich starten",
                L"Avvia l'applicazione ridotta nell'area di notifica",
                L"Iniciar aplicativo minimizado na bandeja"
            },
            {
                "chk_skip_del",
                L"Ne pas demander de confirmation lors de la suppression d'une conversation",
                L"Do not ask for confirmation when deleting a conversation",
                L"No pedir confirmaci\u00f3n al eliminar una conversaci\u00f3n",
                L"Beim L\u00f6schen einer Konversation nicht nach Best\u00e4tigung fragen",
                L"Non chiedere conferma quando si elimina una conversazione",
                L"N\u00e3o pedir confirma\u00e7\u00e3o ao apagar uma conversa"
            },
            {
                "btn_save",
                L"Enregistrer",
                L"Save",
                L"Guardar",
                L"Speichern",
                L"Salva",
                L"Salvar"
            },
            {
                "btn_cancel",
                L"Annuler",
                L"Cancel",
                L"Cancelar",
                L"Abbrechen",
                L"Annulla",
                L"Cancelar"
            },
            {
                "new_sms_title",
                L"Nouveau SMS",
                L"New SMS",
                L"Nuevo SMS",
                L"Neue SMS",
                L"Nuovo SMS",
                L"Novo SMS"
            },
            {
                "new_sms_body",
                L"Nouveau message re\u00e7u",
                L"New message received",
                L"Nuevo mensaje recibido",
                L"Neue Nachricht empfangen",
                L"Nuovo messaggio ricevuto",
                L"Nova mensagem recebida"
            },
            {
                "att_mms",
                L"[Pi\u00e8ce jointe MMS]",
                L"[MMS attachment]",
                L"[Archivo adjunto MMS]",
                L"[MMS-Anhang]",
                L"[Allegato MMS]",
                L"[Anexo MMS]"
            },
            {
                "att_image",
                L"[Image]",
                L"[Image]",
                L"[Imagen]",
                L"[Bild]",
                L"[Immagine]",
                L"[Imagem]"
            },
            {
                "att_video",
                L"[Vid\u00e9o]",
                L"[Video]",
                L"[V\u00eddeo]",
                L"[Video]",
                L"[Video]",
                L"[V\u00eddeo]"
            },
            {
                "thread_fallback",
                L"Fil #{0}",
                L"Thread #{0}",
                L"Hilo #{0}",
                L"Chat #{0}",
                L"Chat #{0}",
                L"Conversa #{0}"
            },
            {
                "conn_waiting",
                L"En attente de connexion du t\u00e9l\u00e9phone sur {0}...",
                L"Waiting for phone connection on {0}...",
                L"Esperando conexi\u00f3n del tel\u00e9fono en {0}...",
                L"Warte auf Telefonverbindung unter {0}...",
                L"In attesa di connessione del telefono su {0}...",
                L"Aguardando conex\u00e3o do telefone em {0}..."
            },
            {
                "conn_inactive",
                L"\u00c9coute r\u00e9seau inactive (v\u00e9rifiez l'IP dans R\u00e9glages)",
                L"Network listener inactive (check IP in Settings)",
                L"Escucha de red inactiva (verifique la IP en Ajustes)",
                L"Netzwerk-Listener inaktiv (IP in den Einstellungen pr\u00fcfen)",
                L"Ascolto di rete inattivo (verifica l'IP nelle Impostazioni)",
                L"Escuta de rede inativa (verifique o IP nas Defini\u00e7\u00f5es)"
            },
            {
                "conn_connected",
                L"Connect\u00e9 \u00e0 {0}{1}",
                L"Connected to {0}{1}",
                L"Conectado a {0}{1}",
                L"Verbunden mit {0}{1}",
                L"Connesso a {0}{1}",
                L"Conectado a {0}{1}"
            },
            {
                "conn_paired",
                L" (Appair\u00e9)",
                L" (Paired)",
                L" (Emparejado)",
                L" (Gekoppelt)",
                L" (Associato)",
                L" (Emparelhado)"
            },
            {
                "conn_pending_pair",
                L" (En attente d'appairage)",
                L" (Pending pairing)",
                L" (Pendiente de emparejamiento)",
                L" (Kopplung ausstehend)",
                L" (In attesa di associazione)",
                L" (Aguardando emparelhamento)"
            },
            {
                "conn_ipc_err",
                L"Service Daemon (IPC) - {0}",
                L"Daemon Service (IPC) - {0}",
                L"Servicio Daemon (IPC) - {0}",
                L"Daemon-Dienst (IPC) - {0}",
                L"Servizio Daemon (IPC) - {0}",
                L"Servi\u00e7o Daemon (IPC) - {0}"
            },
            {
                "conn_ipc_active",
                L"Connect\u00e9 au service Daemon (IPC actif)",
                L"Connected to Daemon service (IPC active)",
                L"Conectado al servicio Daemon (IPC activo)",
                L"Mit Daemon-Dienst verbunden (IPC aktiv)",
                L"Connesso al servizio Daemon (IPC attivo)",
                L"Conectado ao servi\u00e7o Daemon (IPC ativo)"
            },
            {
                "conn_ipc_active_bound",
                L"Connect\u00e9 au service Daemon (IPC actif - {0})",
                L"Connected to Daemon service (IPC active - {0})",
                L"Conectado al servicio Daemon (IPC activo - {0})",
                L"Mit Daemon-Dienst verbunden (IPC aktiv - {0})",
                L"Connesso al servizio Daemon (IPC attivo - {0})",
                L"Conectado ao servi\u00e7o Daemon (IPC ativo - {0})"
            },
            {
                "emoji_picker_title",
                L"\u00c9mojis (ou Touche Windows + .)",
                L"Emojis (or Win + .)",
                L"Emojis (o Win + .)",
                L"Emojis (oder Win + .)",
                L"Emoji (o Win + .)",
                L"Emojis (ou Win + .)"
            },
            {
                "filter_media",
                L"Images et vid\u00e9os (*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp;*.mp4;*.mov)",
                L"Images and videos (*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp;*.mp4;*.mov)",
                L"Im\u00e1genes y v\u00eddeos (*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp;*.mp4;*.mov)",
                L"Bilder und Videos (*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp;*.mp4;*.mov)",
                L"Immagini e video (*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp;*.mp4;*.mov)",
                L"Imagens e v\u00eddeos (*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp;*.mp4;*.mov)"
            },
            {
                "filter_all",
                L"Tous les fichiers (*.*)",
                L"All files (*.*)",
                L"Todos los archivos (*.*)",
                L"Alle Dateien (*.*)",
                L"Tutti i file (*.*)",
                L"Todos os arquivos (*.*)"
            }
        };

        const std::unordered_map<std::string, const LangEntry*>& GetStringTable() {
            static const std::unordered_map<std::string, const LangEntry*> table = []() {
                std::unordered_map<std::string, const LangEntry*> m;
                for (const auto& entry : kStrings) {
                    m.emplace(entry.key, &entry);
                }
                return m;
            }();
            return table;
        }

        std::wstring PickEntryText(const LangEntry& e, AppLanguage lang) {
            switch (lang) {
                case AppLanguage::French:     return e.fr;
                case AppLanguage::Spanish:    return e.es;
                case AppLanguage::German:     return e.de;
                case AppLanguage::Italian:    return e.it;
                case AppLanguage::Portuguese: return e.pt;
                case AppLanguage::English:
                default:                      return e.en;
            }
        }

        void ReplaceAllWide(std::wstring& str, const std::wstring& from, const std::wstring& to) {
            if (from.empty()) return;
            size_t pos = 0;
            while ((pos = str.find(from, pos)) != std::wstring::npos) {
                str.replace(pos, from.size(), to);
                pos += to.size();
            }
        }

    } // namespace

    I18n& I18n::Instance() {
        static I18n instance;
        return instance;
    }

    I18n::I18n() {
        m_effective = DetectSystemLanguage();
    }

    AppLanguage I18n::DetectSystemLanguage() const {
        LANGID langId = GetUserDefaultUILanguage();
        switch (PRIMARYLANGID(langId)) {
            case LANG_FRENCH:     return AppLanguage::French;
            case LANG_SPANISH:    return AppLanguage::Spanish;
            case LANG_GERMAN:     return AppLanguage::German;
            case LANG_ITALIAN:    return AppLanguage::Italian;
            case LANG_PORTUGUESE: return AppLanguage::Portuguese;
            case LANG_ENGLISH:
            default:              return AppLanguage::English;
        }
    }

    void I18n::Initialize() {
        std::wstring code = ReadIniString(L"App", L"Language", L"auto");
        AppLanguage cfg = LanguageFromCode(code);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_configured = cfg;
        m_effective = (cfg == AppLanguage::Auto) ? DetectSystemLanguage() : cfg;
        m_initialized = true;
    }

    AppLanguage I18n::GetConfiguredLanguage() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_configured;
    }

    AppLanguage I18n::GetEffectiveLanguage() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_effective;
    }

    void I18n::SetLanguage(AppLanguage lang) {
        std::wstring code = CodeFromLanguage(lang);
        WriteIniString(L"App", L"Language", code.c_str());
        std::lock_guard<std::mutex> lock(m_mutex);
        m_configured = lang;
        m_effective = (lang == AppLanguage::Auto) ? DetectSystemLanguage() : lang;
        m_initialized = true;
    }

    std::wstring I18n::Translate(const char* key) const {
        if (!key) return L"";
        AppLanguage eff;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_initialized) {
                eff = DetectSystemLanguage();
            } else {
                eff = m_effective;
            }
        }
        const auto& table = GetStringTable();
        auto it = table.find(key);
        if (it != table.end()) {
            return PickEntryText(*it->second, eff);
        }
        return Utf8ToWide(key);
    }

    std::wstring I18n::TranslateFmt(const char* key, const std::wstring& arg0, const std::wstring& arg1) const {
        std::wstring out = Translate(key);
        ReplaceAllWide(out, L"{0}", arg0);
        ReplaceAllWide(out, L"{1}", arg1);
        return out;
    }

    std::wstring I18n::GetLanguageDisplayName(AppLanguage lang) {
        switch (lang) {
            case AppLanguage::Auto:       return Instance().Translate("lang_auto");
            case AppLanguage::French:     return L"Fran\u00e7ais";
            case AppLanguage::English:    return L"English";
            case AppLanguage::Spanish:    return L"Espa\u00f1ol";
            case AppLanguage::German:     return L"Deutsch";
            case AppLanguage::Italian:    return L"Italiano";
            case AppLanguage::Portuguese: return L"Portugu\u00eas";
            default:                      return L"English";
        }
    }

    AppLanguage I18n::LanguageFromCode(const std::wstring& code) {
        std::wstring low = code;
        for (auto& ch : low) {
            ch = static_cast<wchar_t>(::towlower(ch));
        }
        while (!low.empty() && (low.front() == L' ' || low.front() == L'\t')) low.erase(low.begin());
        while (!low.empty() && (low.back() == L' ' || low.back() == L'\t')) low.pop_back();
        if (low == L"fr" || low == L"french" || low == L"francais") return AppLanguage::French;
        if (low == L"en" || low == L"english") return AppLanguage::English;
        if (low == L"es" || low == L"spanish") return AppLanguage::Spanish;
        if (low == L"de" || low == L"german") return AppLanguage::German;
        if (low == L"it" || low == L"italian") return AppLanguage::Italian;
        if (low == L"pt" || low == L"portuguese") return AppLanguage::Portuguese;
        return AppLanguage::Auto;
    }

    std::wstring I18n::CodeFromLanguage(AppLanguage lang) {
        switch (lang) {
            case AppLanguage::French:     return L"fr";
            case AppLanguage::English:    return L"en";
            case AppLanguage::Spanish:    return L"es";
            case AppLanguage::German:     return L"de";
            case AppLanguage::Italian:    return L"it";
            case AppLanguage::Portuguese: return L"pt";
            case AppLanguage::Auto:
            default:                      return L"auto";
        }
    }

} // namespace KDEConnect
