/*
 * ============================================================================
 *  minios.c — MINI SISTEMA OPERATIVO CLI  ·  versión 2.0
 * ----------------------------------------------------------------------------
 *  Compilación estricta (sin warnings):
 *      gcc -Wall -Wextra minios.c -o minios
 *
 *  Estructura:
 *      1. Menú Principal (bucle)        -> 1) Gestor de Archivos
 *                                         2) Centro de Juegos
 *                                         3) Bitácora del Sistema
 *                                         0) Apagar Sistema
 *      2. Gestor de Archivos (11 ops.)  -> solo llamadas POSIX (sin system())
 *      3. Centro de Juegos              -> fork/exec/waitpid + tcgetattr
 *      4. Bitácora                      -> bitacora.txt con timestamps
 *
 *  Seguridad: todas las rutas del Gestor de Archivos pasan por
 *  resolver_ruta(), que normaliza con realpath() y exige que la ruta final
 *  comience estrictamente dentro del disco virtual (anti Path Traversal).
 * ============================================================================
 */

/* Perfil POSIX.1-2008 + XSI (necesario para realpath() en cabeceras newlib) */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE   700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <ctype.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define BLOQUE      4096          /* Tamaño de bloque de lectura/escritura  */
#define NOMBRE_BIT  "bitacora.txt"/* Auditoría del sistema                  */

/* Ruta absoluta (ya normalizada) del disco virtual aislado */
static char g_disco[PATH_MAX];

/* ==========================================================================
 *  UTILIDADES DE ENTRADA (validación robusta de stdin)
 * ========================================================================== */

/* Limpia el buffer stdin hasta el final de línea (o EOF). */
static void limpiar_buffer(void)
{
    int c;
    while ((c = getchar()) != '\n' && c != EOF) {
        /* descarta */
    }
}

/*
 * Lee una línea desde consola.
 * Devuelve 1 si se leyó algo, 0 en EOF (Ctrl+D). Trunca líneas largas.
 */
static int leer_texto(char *dest, size_t cap)
{
    if (fgets(dest, (int)cap, stdin) == NULL) {
        return 0;
    }

    size_t lon = strlen(dest);
    if (lon > 0 && dest[lon - 1] == '\n') {
        dest[lon - 1] = '\0';
    } else if (!feof(stdin)) {
        limpiar_buffer(); /* la línea excede el buffer: se descarta el resto */
        printf("[AVISO] Línea demasiado larga; se ha truncado.\n");
    }
    return 1;
}

/*
 * Lee y convierte una opción numérica.
 *   1  -> valor válido almacenado en *destino
 *   0  -> EOF (entrada cerrada)
 *  -1  -> entrada no numérica o fuera de rango de int (buffer ya limpio)
 */
static int leer_entero(const char *mensaje, int *destino)
{
    char linea[64];
    char *fin;
    long valor;

    printf("%s", mensaje);
    fflush(stdout);

    if (!leer_texto(linea, sizeof(linea))) {
        return 0;
    }

    errno = 0;
    valor = strtol(linea, &fin, 10);
    while (*fin == ' ' || *fin == '\t') {
        fin++;
    }
    if (fin == linea || *fin != '\0' || errno == ERANGE ||
        valor < (long)INT_MIN || valor > (long)INT_MAX) {
        return -1;
    }

    *destino = (int)valor;
    return 1;
}

/* Pausa amigable: espera a que el usuario presione ENTER. */
static void pausa(void)
{
    char buf[8];
    printf("\nPresione ENTER para continuar...");
    fflush(stdout);
    if (!leer_texto(buf, sizeof(buf))) {
        printf("\n");
    }
}

/* ==========================================================================
 *  INTERFAZ — limpieza de consola y encabezados (ANSI, sin system())
 * ========================================================================== */

/*
 * Limpia la pantalla con la secuencia ANSI estándar de Linux:
 *   \033[H  -> mueve el cursor al origen (fila 1, columna 1)
 *   \033[J  -> borra desde el cursor hasta el final de la pantalla
 * Equivale a system("clear") SIN invocar system() (prohibido por el proyecto).
 */
static void limpiar_pantalla(void)
{
    printf("\033[H\033[J");
    fflush(stdout);
}

/*
 * Limpia la consola y dibuja un encabezado destacado (negrita) con el módulo
 * y la opción que se está ejecutando, por ejemplo:
 *      ==================================================================
 *         === GESTOR DE ARCHIVOS > CREAR ARCHIVO ===
 *      ==================================================================
 * Si `opcion` es NULL solo se muestra el módulo.
 */
static void encabezado(const char *modulo, const char *opcion)
{
    limpiar_pantalla();
    printf("\033[1m"); /* negrita: título destacado */
    printf("==================================================================\n");
    if (opcion != NULL && opcion[0] != '\0') {
        printf("   === %s > %s ===\n", modulo, opcion);
    } else {
        printf("   === %s ===\n", modulo);
    }
    printf("==================================================================\n");
    printf("\033[0m");  /* restablece atributos al valor por defecto */
    fflush(stdout);
}

/* ==========================================================================
 *  BITÁCORA DEL SISTEMA (auditoría con marca de tiempo)
 * ========================================================================== */

static int ruta_bitacora(char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/%s", g_disco, NOMBRE_BIT);
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    return 0;
}

/* Registra un evento de auditoría en bitacora.txt (append + timestamp). */
static void registrar(const char *formato, ...)
{
    char ruta[PATH_MAX];
    char marca[64];
    char mensaje[768];
    char linea[896];
    time_t ahora;
    struct tm *tm;
    va_list ap;
    int fd;
    ssize_t escrito;

    if (ruta_bitacora(ruta, sizeof(ruta)) != 0) {
        return;
    }

    ahora = time(NULL);
    tm = localtime(&ahora);
    if (tm != NULL) {
        if (strftime(marca, sizeof(marca), "%Y-%m-%d %H:%M:%S", tm) == 0) {
            strcpy(marca, "????");
        }
    } else {
        strcpy(marca, "????");
    }

    va_start(ap, formato);
    vsnprintf(mensaje, sizeof(mensaje), formato, ap);
    va_end(ap);

    snprintf(linea, sizeof(linea), "[%s] %s\n", marca, mensaje);

    fd = open(ruta, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        fprintf(stderr, "[BITACORA] No se pudo escribir: %s\n", strerror(errno));
        return;
    }
    escrito = write(fd, linea, strlen(linea));
    (void)escrito;
    close(fd);
}

/* Opción 3 del menú principal: muestra el contenido de la bitácora. */
static void ver_bitacora(void)
{
    char ruta[PATH_MAX];
    char buf[BLOQUE];
    int fd;
    ssize_t n;

    if (ruta_bitacora(ruta, sizeof(ruta)) != 0) {
        printf("[ERROR] Ruta de bitácora inválida.\n");
        return;
    }

    fd = open(ruta, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            printf("[INFO] Todavía no hay eventos registrados.\n");
        } else {
            printf("[ERROR] No se pudo abrir la bitácora: %s\n", strerror(errno));
        }
        return;
    }

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n) {
            fprintf(stderr, "\n[ERROR] Fallo al escribir en la consola.\n");
            break;
        }
    }
    if (n < 0) {
        printf("[ERROR] Error de lectura de la bitácora: %s\n", strerror(errno));
    }
    close(fd);
    printf("\nFin de la bitácora.\n");
}

/* ==========================================================================
 *  INICIALIZACIÓN DEL DISCO VIRTUAL Y SANDBOXING (anti path traversal)
 * ========================================================================== */

/* Crea un directorio y todos sus padres (equivalente a mkdir -p). */
static int crear_directorios(const char *ruta)
{
    char tmp[PATH_MAX];
    size_t lon;
    size_t i;

    lon = strlen(ruta);
    if (lon == 0 || lon >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(tmp, ruta, lon + 1);

    for (i = 1; i < lon; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/*
 * Determina la ruta absoluta del disco virtual.
 * Preferencia: $HOME/miniSO/disk  →  ./disk  →  crear la primera.
 */
static int inicializar_disco(void)
{
    const char *home = getenv("HOME");
    char candidato[PATH_MAX];
    int n;

    if (home != NULL && home[0] != '\0') {
        n = snprintf(candidato, sizeof(candidato), "%s/miniSO/disk", home);
        if (n > 0 && (size_t)n < sizeof(candidato)) {
            if (realpath(candidato, g_disco) != NULL) {
                goto normalizar;           /* ya existe: se usa tal cual */
            }
            if (crear_directorios(candidato) == 0 &&
                realpath(candidato, g_disco) != NULL) {
                goto normalizar;           /* recién creado */
            }
            fprintf(stderr, "[AVISO] No se pudo usar %s (%s).\n",
                    candidato, strerror(errno));
        }
    }

    /* Alternativa: ./disk en el directorio de trabajo actual */
    if (realpath("./disk", g_disco) != NULL) {
        goto normalizar;
    }
    if (crear_directorios("./disk") != 0) {
        fprintf(stderr, "[ERROR] No se pudo crear el disco virtual: %s\n",
                strerror(errno));
        return -1;
    }
    if (realpath("./disk", g_disco) == NULL) {
        fprintf(stderr, "[ERROR] realpath(./disk): %s\n", strerror(errno));
        return -1;
    }

normalizar:
    /* Se eliminan barras finales para que la comparación de prefijo sea exacta */
    {
        size_t lon = strlen(g_disco);
        while (lon > 1 && g_disco[lon - 1] == '/') {
            g_disco[--lon] = '\0';
        }
    }
    return 0;
}

/*
 * Normalización puramente léxica: colapsa "." y ".." sin tocar el sistema de
 * archivos. Sirve para detectar intentos de fuga aunque los directorios
 * intermedios aún no existan (realpath fallaría con ENOENT).
 * Devuelve 0 con la ruta absoluta normalizada en `salida`.
 */
static int normalizar_lexica(const char *ruta, char *salida, size_t cap)
{
    const char *p = ruta;
    size_t n = 0;

    if (ruta[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    salida[n++] = '/';

    while (*p != '\0') {
        const char *ini;
        size_t comp;

        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        ini = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        comp = (size_t)(p - ini);

        if (comp == 1 && ini[0] == '.') {
            continue;
        }
        if (comp == 2 && ini[0] == '.' && ini[1] == '.') {
            while (n > 1 && salida[n - 1] != '/') {
                n--;
            }
            if (n > 1) {
                n--; /* se elimina la '/' sobrante del componente anterior */
            }
            continue;
        }
        if (n > 1) {
            if (n + 1 + comp >= cap) {
                errno = ENAMETOOLONG;
                return -1;
            }
            salida[n++] = '/';
        } else if (n + comp >= cap) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(salida + n, ini, comp);
        n += comp;
    }

    salida[n] = '\0';
    return 0;
}

/*
 * SANDBOX / JAIL: convierte una ruta escrita por el usuario en una ruta
 * absoluta normalizada que DEBE quedar estrictamente dentro de g_disco.
 *
 *  1) Normalización léxica + comprobación de prefijo (bloquea "../../etc/…"
 *     aunque los directorios intermedios no existan).
 *  2) realpath() colapsa rutas reales y enlaces simbólicos (bloquea escapes
 *     por symlink a lugares ya existentes).
 *  3) Si la ruta aún no existe (archivo nuevo), se resuelve el padre y se
 *     añade el nombre final (validando que no sea "." ni "..").
 *
 * Devuelve 0 si la ruta es válida (ya copiada en `salida`), -1 si no.
 */
static int resolver_ruta(const char *entrada, char *salida, size_t cap)
{
    char base[PATH_MAX];
    char lexica[PATH_MAX];
    size_t lon;
    size_t raiz;

    if (cap < PATH_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (entrada == NULL || entrada[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    /* Se compone la ruta candidata (absoluta o relativa al disco) */
    if (entrada[0] == '/') {
        lon = (size_t)snprintf(base, sizeof(base), "%s", entrada);
    } else {
        lon = (size_t)snprintf(base, sizeof(base), "%s/%s", g_disco, entrada);
    }
    if (lon >= sizeof(base)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    /* Se recortan barras finales ("dir/" -> "dir") */
    while (lon > 1 && base[lon - 1] == '/') {
        base[--lon] = '\0';
    }

    raiz = strlen(g_disco);

    /* 1) Filtro léxico: rechaza de inmediato cualquier ".." que salga */
    if (normalizar_lexica(base, lexica, sizeof(lexica)) != 0) {
        return -1;
    }
    if (strncmp(lexica, g_disco, raiz) != 0 ||
        (lexica[raiz] != '\0' && lexica[raiz] != '/')) {
        errno = EPERM;
        return -1;
    }

    /* 2) Resolución real (colapsa enlaces simbólicos) */
    if (realpath(base, salida) == NULL) {
        char dirpadre[PATH_MAX];
        char nombre[PATH_MAX];
        char resuelto[PATH_MAX];
        char *slash;

        if (errno != ENOENT) {
            return -1; /* EACCES, ENOTDIR, ELOOP, etc. */
        }

        /* La ruta no existe aún: resolvemos el directorio padre */
        snprintf(dirpadre, sizeof(dirpadre), "%s", base);
        slash = strrchr(dirpadre, '/');
        if (slash == NULL) {
            errno = EINVAL;
            return -1;
        }
        if (slash == dirpadre) {
            slash[1] = '\0'; /* raíz del sistema */
        } else {
            *slash = '\0';
        }

        snprintf(nombre, sizeof(nombre), "%s", strrchr(base, '/') + 1);
        if (nombre[0] == '\0' ||
            strcmp(nombre, ".") == 0 || strcmp(nombre, "..") == 0) {
            errno = EINVAL;
            return -1;
        }

        if (realpath(dirpadre, resuelto) == NULL) {
            return -1;
        }
        if (snprintf(salida, cap, "%s/%s", resuelto, nombre) >= (int)cap) {
            errno = ENAMETOOLONG;
            return -1;
        }
    }

    /* 3) Comprobación estricta de aislamiento dentro del disco virtual */
    if (strncmp(salida, g_disco, raiz) != 0 ||
        (salida[raiz] != '\0' && salida[raiz] != '/')) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

/* Pide una ruta/nombre al usuario y la valida contra el sandbox. */
static int pedir_ruta(const char *mensaje, char *salida, size_t cap)
{
    char entrada[PATH_MAX];

    printf("%s", mensaje);
    fflush(stdout);
    if (!leer_texto(entrada, sizeof(entrada))) {
        return 0;
    }
    if (entrada[0] == '\0') {
        printf("[ERROR] La ruta no puede estar vacía.\n");
        return -1;
    }
    if (resolver_ruta(entrada, salida, cap) != 0) {
        if (errno == EPERM) {
            printf("[SEGURIDAD] Ruta rechazada: queda fuera del disco virtual (%s).\n",
                   g_disco);
        } else {
            printf("[ERROR] Ruta inválida: %s\n", strerror(errno));
        }
        registrar("ERROR ruta rechazada '%s': %s", entrada, strerror(errno));
        return -1;
    }
    return 1;
}

/* ==========================================================================
 *  AYUDANTES DE FORMATO (permisos, fechas, tipo, volcado hexadecimal)
 * ========================================================================== */

/* "d"/"-"/"l" + rwxrwxrwx  (10 caracteres + '\0') */
static void permisos_texto(mode_t m, char *out)
{
    out[0] = S_ISDIR(m) ? 'd' : (S_ISLNK(m) ? 'l' : '-');
    out[1] = (m & S_IRUSR) ? 'r' : '-';
    out[2] = (m & S_IWUSR) ? 'w' : '-';
    out[3] = (m & S_IXUSR) ? 'x' : '-';
    out[4] = (m & S_IRGRP) ? 'r' : '-';
    out[5] = (m & S_IWGRP) ? 'w' : '-';
    out[6] = (m & S_IXGRP) ? 'x' : '-';
    out[7] = (m & S_IROTH) ? 'r' : '-';
    out[8] = (m & S_IWOTH) ? 'w' : '-';
    out[9] = (m & S_IXOTH) ? 'x' : '-';
    out[10] = '\0';
}

static const char *tipo_texto(mode_t m)
{
    if (S_ISDIR(m))  return "directorio";
    if (S_ISLNK(m))  return "enlace";
    if (S_ISREG(m))  return "archivo";
    return "especial";
}

static void fecha_texto(time_t t, char *out, size_t cap)
{
    struct tm *tm = localtime(&t);
    if (tm == NULL || strftime(out, cap, "%Y-%m-%d %H:%M:%S", tm) == 0) {
        snprintf(out, cap, "desconocida");
    }
}

/* Vuelca en hexadecimal (%02X) los primeros 64 bytes indicados. */
static void volcado_hex(const unsigned char *datos, size_t n)
{
    size_t i, j;

    for (i = 0; i < n; i += 16) {
        printf("%08zX  ", i);
        for (j = 0; j < 16; j++) {
            if (i + j < n) {
                printf("%02X ", (unsigned int)datos[i + j]);
            } else {
                printf("   ");
            }
            if (j == 7) {
                printf(" ");
            }
        }
        printf(" |");
        for (j = 0; j < 16 && i + j < n; j++) {
            unsigned char c = datos[i + j];
            printf("%c", (c >= 0x20 && c < 0x7F) ? (char)c : '.');
        }
        printf("|\n");
    }
}

/* Heurística de "¿es texto plano?": sin NUL ni controles extraños. */
static int es_texto(const unsigned char *d, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned char c = d[i];
        if (c == 0x00 ||
            (c < 0x09) ||
            (c > 0x0D && c < 0x20) ||
            c == 0x7F) {
            return 0;
        }
    }
    return 1;
}

/* Comparación de cadenas sin distinguir mayúsculas. */
static int contiene_ci(const char *texto, const char *patron)
{
    size_t lon = strlen(patron);
    const char *p;

    if (lon == 0) {
        return 1;
    }
    for (p = texto; *p != '\0'; p++) {
        size_t i = 0;
        while (i < lon && p[i] != '\0' &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)patron[i])) {
            i++;
        }
        if (i == lon) {
            return 1;
        }
    }
    return 0;
}

/* ==========================================================================
 *  GESTOR DE ARCHIVOS — 11 operaciones (solo POSIX, SIN system())
 * ========================================================================== */

/* 1. LISTAR directorio: nombre, tipo, tamaño, permisos y fecha. */
static void fs_listar(void)
{
    DIR *dir;
    struct dirent *ent;
    int total = 0;

    dir = opendir(g_disco);
    if (dir == NULL) {
        printf("[ERROR] opendir(%s): %s\n", g_disco, strerror(errno));
        registrar("ERROR opendir: %s", strerror(errno));
        return;
    }

    printf("\nContenido de [%s]\n", g_disco);
    printf("%-34s %-11s %13s  %-11s %-9s %s\n",
           "NOMBRE", "TIPO", "TAMAÑO (B)", "PERMISOS", "OCTAL", "MODIFICADO");
    printf("----------------------------------------------------------------------------------------------------\n");

    while ((ent = readdir(dir)) != NULL) {
        char ruta[PATH_MAX];
        struct stat st;
        char perm[12];
        char fecha[32];

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (snprintf(ruta, sizeof(ruta), "%s/%s", g_disco, ent->d_name)
                >= (int)sizeof(ruta)) {
            printf("[AVISO] Ruta demasiado larga, omitida.\n");
            continue;
        }
        if (lstat(ruta, &st) != 0) {
            printf("[AVISO] No se pudo stat '%s': %s\n", ent->d_name, strerror(errno));
            continue;
        }

        permisos_texto(st.st_mode, perm);
        fecha_texto(st.st_mtime, fecha, sizeof(fecha));

        printf("%-34s %-11s %13lld  %-11s %03o       %s\n",
               ent->d_name,
               tipo_texto(st.st_mode),
               (long long)st.st_size,
               perm,
               (unsigned int)(st.st_mode & 0777),
               fecha);
        total++;
    }
    closedir(dir);

    if (total == 0) {
        printf("[INFO] El directorio está vacío.\n");
    } else {
        printf("----------------------------------------------------------------------------------------------------\n");
        printf("Elementos listados: %d\n", total);
    }
    registrar("LISTAR directorio: %d elemento(s)", total);
}

/* Escribe por bloques de consola hasta una línea con solo '.' o EOF. */
static ssize_t escribir_contenido(int fd)
{
    char linea[1024];
    ssize_t total = 0;

    printf("Ingrese el contenido (finalice con una línea que contenga solo '.'): \n");
    for (;;) {
        printf("> ");
        fflush(stdout);
        if (!leer_texto(linea, sizeof(linea))) {
            printf("\n");
            break;
        }
        if (strcmp(linea, ".") == 0) {
            break;
        }
        {
            size_t lon = strlen(linea);
            linea[lon++] = '\n';
            if (write(fd, linea, lon) != (ssize_t)lon) {
                return -1;
            }
            total += (ssize_t)lon;
        }
    }
    return total;
}

/* 2. CREAR ARCHIVO DE TEXTO. */
static void fs_crear_archivo(void)
{
    char ruta[PATH_MAX];
    int fd;
    ssize_t bytes;
    int r = pedir_ruta("Nombre del archivo a crear: ", ruta, sizeof(ruta));

    if (r != 1) {
        return;
    }

    fd = open(ruta, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("[ERROR] open(%s): %s\n", ruta, strerror(errno));
        registrar("ERROR crear archivo '%s': %s", ruta, strerror(errno));
        return;
    }

    bytes = escribir_contenido(fd);
    if (bytes < 0) {
        printf("[ERROR] write: %s\n", strerror(errno));
        registrar("ERROR escribir archivo '%s': %s", ruta, strerror(errno));
    } else {
        printf("[ÉXITO] Archivo creado con %lld byte(s).\n", (long long)bytes);
        registrar("CREAR archivo '%s' (%lld byte(s))", ruta, (long long)bytes);
    }
    close(fd);
}

/* 3. CREAR DIRECTORIO (mkdir). */
static void fs_crear_directorio(void)
{
    char ruta[PATH_MAX];
    int r = pedir_ruta("Nombre del directorio a crear: ", ruta, sizeof(ruta));

    if (r != 1) {
        return;
    }
    if (mkdir(ruta, 0755) != 0) {
        printf("[ERROR] mkdir: %s\n", strerror(errno));
        registrar("ERROR mkdir '%s': %s", ruta, strerror(errno));
        return;
    }
    printf("[ÉXITO] Directorio '%s' creado.\n", ruta);
    registrar("CREAR directorio '%s'", ruta);
}

/* 4. LEER ARCHIVO: texto completo o metadatos + primeros 64 bytes en hex. */
static void fs_leer_archivo(void)
{
    char ruta[PATH_MAX];
    char buf[BLOQUE];
    struct stat st;
    int fd;
    ssize_t n;
    int r = pedir_ruta("Archivo a leer: ", ruta, sizeof(ruta));

    if (r != 1) {
        return;
    }

    fd = open(ruta, O_RDONLY);
    if (fd < 0) {
        printf("[ERROR] open: %s\n", strerror(errno));
        registrar("ERROR leer '%s': %s", ruta, strerror(errno));
        return;
    }
    if (fstat(fd, &st) != 0) {
        printf("[ERROR] stat: %s\n", strerror(errno));
        close(fd);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        printf("[ERROR] '%s' es un directorio; use la opción 1 para listarla.\n", ruta);
        close(fd);
        return;
    }

    n = read(fd, buf, sizeof(buf));
    if (n < 0) {
        printf("[ERROR] read: %s\n", strerror(errno));
        registrar("ERROR read '%s': %s", ruta, strerror(errno));
        close(fd);
        return;
    }

    if (es_texto((unsigned char *)buf, (size_t)n)) {
        size_t restante = (size_t)n;
        char *p = buf;

        printf("\n----- CONTENIDO DE TEXTO: %s (%lld byte(s)) -----\n",
               ruta, (long long)st.st_size);
        while (restante > 0) {
            if (fwrite(p, 1, restante, stdout) != restante) {
                fprintf(stderr, "\n[ERROR] Fallo de salida en consola.\n");
                break;
            }
            restante = 0;
            n = read(fd, buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            restante = (size_t)n;
            p = buf;
        }
        printf("\n-----------------------------------------------------\n");
        registrar("LEER archivo de texto '%s'", ruta);
    } else {
        char perm[12];
        char fecha[32];
        size_t muestra = ((size_t)n < 64) ? (size_t)n : 64;

        printf("\n----- ARCHIVO BINARIO / NO TEXTO: %s -----\n", ruta);
        printf("Tipo        : %s\n", tipo_texto(st.st_mode));
        printf("Tamaño      : %lld bytes\n", (long long)st.st_size);
        printf("Permisos    : ");
        permisos_texto(st.st_mode, perm);
        printf("%s (%03o)\n", perm, (unsigned int)(st.st_mode & 0777));
        fecha_texto(st.st_mtime, fecha, sizeof(fecha));
        printf("Modificado  : %s\n", fecha);
        printf("UID/GID     : %ld / %ld\n", (long)st.st_uid, (long)st.st_gid);
        printf("\nPrimeros %zu byte(s) en hexadecimal:\n", muestra);
        volcado_hex((unsigned char *)buf, muestra);
        printf("-----------------------------------------------------\n");
        registrar("LEER archivo binario '%s' (%lld byte(s))",
                  ruta, (long long)st.st_size);
    }
    close(fd);
}

/* 5. EDITAR ARCHIVO DE TEXTO: anexar (append) o sobrescribir. */
static void fs_editar_archivo(void)
{
    char ruta[PATH_MAX];
    char sub[32];
    int modo;
    int fd;
    ssize_t bytes;
    int r = pedir_ruta("Archivo de texto a editar: ", ruta, sizeof(ruta));

    if (r != 1) {
        return;
    }

    printf("1. Anexar contenido al final (append)\n");
    printf("2. Sobrescribir el archivo por completo\n");
    printf("Seleccione (1-2): ");
    fflush(stdout);
    if (!leer_texto(sub, sizeof(sub))) {
        return;
    }
    modo = atoi(sub);
    if (modo != 1 && modo != 2) {
        printf("[ERROR] Opción inválida; se cancela la edición.\n");
        return;
    }

    if (modo == 1) {
        fd = open(ruta, O_WRONLY | O_CREAT | O_APPEND, 0644);
    } else {
        fd = open(ruta, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (fd < 0) {
        printf("[ERROR] open: %s\n", strerror(errno));
        registrar("ERROR editar '%s': %s", ruta, strerror(errno));
        return;
    }

    bytes = escribir_contenido(fd);
    if (bytes < 0) {
        printf("[ERROR] write: %s\n", strerror(errno));
        registrar("ERROR editar '%s': %s", ruta, strerror(errno));
    } else {
        printf("[ÉXITO] %s realizada: %lld byte(s) escritos.\n",
               (modo == 1) ? "Anexación" : "Sobrescritura", (long long)bytes);
        registrar("EDITAR '%s' (%s, %lld byte(s))", ruta,
                  (modo == 1) ? "append" : "overwrite", (long long)bytes);
    }
    close(fd);
}

/* Compara tamaño y contenido byte a byte de dos archivos. */
static int verificar_integridad(const char *origen, const char *destino)
{
    struct stat a, b;
    int fa, fb;
    unsigned char ba[BLOQUE];
    unsigned char bb[BLOQUE];
    int identico = 1;

    if (stat(origen, &a) != 0 || stat(destino, &b) != 0) {
        printf("[ERROR] No se pudieron comparar los tamaños: %s\n", strerror(errno));
        return 0;
    }
    if (a.st_size != b.st_size) {
        printf("[INTEGRIDAD] FALLO: tamaños distintos (%lld != %lld).\n",
               (long long)a.st_size, (long long)b.st_size);
        return 0;
    }
    printf("[INTEGRIDAD] Tamaño idéntico: %lld byte(s).\n", (long long)a.st_size);

    fa = open(origen, O_RDONLY);
    fb = open(destino, O_RDONLY);
    if (fa < 0 || fb < 0) {
        printf("[ERROR] No se pudieron reabrir los archivos: %s\n", strerror(errno));
        if (fa >= 0) close(fa);
        if (fb >= 0) close(fb);
        return 0;
    }

    for (;;) {
        ssize_t na = read(fa, ba, sizeof(ba));
        ssize_t nb = read(fb, bb, sizeof(bb));

        if (na < 0 || nb < 0) {
            printf("[ERROR] Error durante la comparación: %s\n", strerror(errno));
            identico = 0;
            break;
        }
        if (na != nb) {
            identico = 0;
            break;
        }
        if (na == 0) {
            break;
        }
        if (memcmp(ba, bb, (size_t)na) != 0) {
            identico = 0;
            break;
        }
    }
    close(fa);
    close(fb);
    return identico;
}

/* 6. COPIAR ARCHIVO en bloques de 4096 + verificación de integridad. */
static void fs_copiar(void)
{
    char origen[PATH_MAX];
    char destino[PATH_MAX];
    char entrada[PATH_MAX];
    unsigned char buf[BLOQUE];
    int fo, fd;
    ssize_t n;
    long long total = 0;
    int r = pedir_ruta("Archivo origen: ", origen, sizeof(origen));

    if (r != 1) {
        return;
    }
    {
        struct stat st;
        if (stat(origen, &st) != 0) {
            printf("[ERROR] stat(origen): %s\n", strerror(errno));
            registrar("ERROR copiar (origen inexistente) '%s'", origen);
            return;
        }
        if (S_ISDIR(st.st_mode)) {
            printf("[ERROR] No se puede copiar un directorio con esta opción.\n");
            return;
        }
    }

    printf("Ruta destino (nombre del duplicado): ");
    fflush(stdout);
    if (!leer_texto(entrada, sizeof(entrada)) || entrada[0] == '\0') {
        printf("[ERROR] Destino vacío; se cancela.\n");
        return;
    }
    if (resolver_ruta(entrada, destino, sizeof(destino)) != 0) {
        printf("[SEGURIDAD/ERROR] Destino rechazado: %s\n", strerror(errno));
        registrar("ERROR copiar destino rechazado '%s': %s", entrada, strerror(errno));
        return;
    }
    if (strcmp(origen, destino) == 0) {
        printf("[ERROR] El destino no puede ser el mismo archivo origen.\n");
        return;
    }

    fo = open(origen, O_RDONLY);
    if (fo < 0) {
        printf("[ERROR] open(origen): %s\n", strerror(errno));
        return;
    }
    fd = open(destino, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("[ERROR] open(destino): %s\n", strerror(errno));
        close(fo);
        return;
    }

    while ((n = read(fo, buf, sizeof(buf))) > 0) {
        if (write(fd, buf, (size_t)n) != n) {
            printf("[ERROR] write durante la copia: %s\n", strerror(errno));
            close(fo);
            close(fd);
            registrar("ERROR copiar '%s' -> '%s': %s", origen, destino, strerror(errno));
            return;
        }
        total += n;
    }
    if (n < 0) {
        printf("[ERROR] read durante la copia: %s\n", strerror(errno));
        close(fo);
        close(fd);
        return;
    }
    close(fo);
    close(fd);

    printf("[COPIA] Escritos %lld byte(s) en '%s'.\n", total, destino);
    printf("[INTEGRIDAD] Comparando original vs copia (tamaño y byte a byte)...\n");
    if (verificar_integridad(origen, destino)) {
        printf("[INTEGRIDAD] ÉXITO: la copia es idéntica al original.\n");
        registrar("COPIAR '%s' -> '%s' (%lld byte(s)) [idéntica]",
                  origen, destino, total);
    } else {
        printf("[INTEGRIDAD] FALLO: la copia NO coincide con el original.\n");
        registrar("COPIAR '%s' -> '%s' [INTEGRIDAD FALLIDA]", origen, destino);
    }
}

/* 7. RENOMBRAR / MOVER con rename(). */
static void fs_renombrar(void)
{
    char origen[PATH_MAX];
    char destino[PATH_MAX];
    char entrada[PATH_MAX];
    int r = pedir_ruta("Ruta actual del archivo/directorio: ", origen, sizeof(origen));

    if (r != 1) {
        return;
    }
    printf("Nueva ruta (nombre o destino): ");
    fflush(stdout);
    if (!leer_texto(entrada, sizeof(entrada)) || entrada[0] == '\0') {
        printf("[ERROR] Nombre vacío; se cancela.\n");
        return;
    }
    if (resolver_ruta(entrada, destino, sizeof(destino)) != 0) {
        printf("[SEGURIDAD/ERROR] Nuevo nombre rechazado: %s\n", strerror(errno));
        registrar("ERROR renombrar destino rechazado '%s'", entrada);
        return;
    }

    if (rename(origen, destino) != 0) {
        printf("[ERROR] rename: %s\n", strerror(errno));
        registrar("ERROR renombrar '%s' -> '%s': %s", origen, destino, strerror(errno));
        return;
    }
    printf("[ÉXITO] Movido/renombrado a '%s'.\n", destino);
    registrar("RENOMBRAR '%s' -> '%s'", origen, destino);
}

/* 8. CAMBIAR PERMISOS con chmod() usando máscara octal. */
static void fs_permisos(void)
{
    char ruta[PATH_MAX];
    char entrada[32];
    char *fin;
    long mascara;
    int r = pedir_ruta("Archivo/directorio a modificar: ", ruta, sizeof(ruta));

    if (r != 1) {
        return;
    }

    printf("Máscara octal (ej.: 755, 644, 777): ");
    fflush(stdout);
    if (!leer_texto(entrada, sizeof(entrada))) {
        return;
    }
    errno = 0;
    mascara = strtol(entrada, &fin, 8);
    if (fin == entrada || *fin != '\0' || errno == ERANGE ||
        mascara < 0 || mascara > 07777) {
        printf("[ERROR] Máscara inválida; use 3 dígitos octales (0-7777).\n");
        return;
    }

    if (chmod(ruta, (mode_t)mascara) != 0) {
        printf("[ERROR] chmod: %s\n", strerror(errno));
        registrar("ERROR chmod '%s': %s", ruta, strerror(errno));
        return;
    }
    {
        char perm[12];
        struct stat st;
        if (stat(ruta, &st) == 0) {
            permisos_texto(st.st_mode, perm);
            printf("[ÉXITO] Permisos de '%s': %s (%03o)\n",
                   ruta, perm, (unsigned int)(st.st_mode & 0777));
        } else {
            printf("[ÉXITO] Permisos establecidos en %03lo\n",
                   (unsigned long)mascara);
        }
    }
    registrar("CHMOD '%s' -> %03lo", ruta, (unsigned long)mascara);
}

/* 9. BUSCAR ARCHIVOS por nombre o extensión dentro del disco. */
static void fs_buscar(void)
{
    char patron[256];
    DIR *dir;
    struct dirent *ent;
    int encontrados = 0;
    int por_extension;

    printf("Patrón a buscar (texto = nombre; '.' inicial = extensión): ");
    fflush(stdout);
    if (!leer_texto(patron, sizeof(patron)) || patron[0] == '\0') {
        printf("[ERROR] Patrón vacío.\n");
        return;
    }
    por_extension = (patron[0] == '.');

    dir = opendir(g_disco);
    if (dir == NULL) {
        printf("[ERROR] opendir: %s\n", strerror(errno));
        return;
    }

    printf("\nResultados para '%s' en [%s]:\n", patron, g_disco);
    while ((ent = readdir(dir)) != NULL) {
        char ruta[PATH_MAX];
        struct stat st;
        int coincide = 0;
        size_t lon_nom;
        size_t lon_pat;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        if (por_extension) {
            lon_nom = strlen(ent->d_name);
            lon_pat = strlen(patron);
            if (lon_nom >= lon_pat) {
                const char *sufijo = ent->d_name + (lon_nom - lon_pat);
                size_t i;
                coincide = 1;
                for (i = 0; i < lon_pat; i++) {
                    if (tolower((unsigned char)sufijo[i]) !=
                        tolower((unsigned char)patron[i])) {
                        coincide = 0;
                        break;
                    }
                }
            }
        } else {
            coincide = contiene_ci(ent->d_name, patron);
        }
        if (!coincide) {
            continue;
        }

        if (snprintf(ruta, sizeof(ruta), "%s/%s", g_disco, ent->d_name)
                >= (int)sizeof(ruta)) {
            continue;
        }
        if (lstat(ruta, &st) != 0) {
            printf("  %-30s (sin metadatos: %s)\n", ent->d_name, strerror(errno));
        } else {
            printf("  %-30s %-11s %12lld B\n", ent->d_name,
                   tipo_texto(st.st_mode), (long long)st.st_size);
        }
        encontrados++;
    }
    closedir(dir);

    if (encontrados == 0) {
        printf("[INFO] Sin coincidencias.\n");
    } else {
        printf("Total de coincidencias: %d\n", encontrados);
    }
    registrar("BUSCAR '%s': %d coincidencia(s)", patron, encontrados);
}

/* 10. ELIMINAR archivo (unlink) o directorio vacío (rmdir) con confirmación. */
static void fs_eliminar(void)
{
    char ruta[PATH_MAX];
    char confirmacion[16];
    struct stat st;
    int r = pedir_ruta("Elemento a eliminar: ", ruta, sizeof(ruta));

    if (r != 1) {
        return;
    }
    if (strcmp(ruta, g_disco) == 0) {
        printf("[SEGURIDAD] No se puede eliminar el disco virtual completo.\n");
        return;
    }
    if (lstat(ruta, &st) != 0) {
        printf("[ERROR] No existe o no se pudo acceder: %s\n", strerror(errno));
        registrar("ERROR eliminar '%s': %s", ruta, strerror(errno));
        return;
    }

    printf("¿Seguro que desea eliminar '%s'? (S/N): ", ruta);
    fflush(stdout);
    if (!leer_texto(confirmacion, sizeof(confirmacion))) {
        return;
    }
    if (confirmacion[0] != 'S' && confirmacion[0] != 's' &&
        confirmacion[0] != 'Y' && confirmacion[0] != 'y') {
        printf("[INFO] Operación cancelada por el usuario.\n");
        registrar("ELIMINAR cancelado '%s'", ruta);
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        if (rmdir(ruta) != 0) {
            printf("[ERROR] rmdir: %s\n", strerror(errno));
            registrar("ERROR rmdir '%s': %s", ruta, strerror(errno));
            return;
        }
        printf("[ÉXITO] Directorio '%s' eliminado.\n", ruta);
        registrar("ELIMINAR directorio '%s'", ruta);
    } else {
        if (unlink(ruta) != 0) {
            printf("[ERROR] unlink: %s\n", strerror(errno));
            registrar("ERROR unlink '%s': %s", ruta, strerror(errno));
            return;
        }
        printf("[ÉXITO] Archivo '%s' eliminado.\n", ruta);
        registrar("ELIMINAR archivo '%s'", ruta);
    }
}

/*
 * Despacha una operación del Gestor de Archivos con la interfaz completa:
 * limpia la consola, muestra el encabezado del módulo > opción, ejecuta la
 * operación y espera con ENTER antes de redibujar el menú.
 */
static void ejecutar_op(const char *titulo, void (*operacion)(void))
{
    encabezado("GESTOR DE ARCHIVOS", titulo);
    operacion();
    pausa();
}

/* Submenú del Gestor de Archivos (bucle propio, opción 11 regresa). */
static void submenu_archivos(void)
{
    int op = 0;
    char aviso[200] = ""; /* último error: se conserva al redibujar el menú */

    for (;;) {
        encabezado("GESTOR DE ARCHIVOS", NULL); /* limpia y titula el menú */
        printf("Disco virtual: %s\n\n", g_disco);
        printf("  1.  Listar contenido del directorio\n");
        printf("  2.  Crear archivo de texto\n");
        printf("  3.  Crear directorio\n");
        printf("  4.  Leer archivo (texto / hex)\n");
        printf("  5.  Editar archivo de texto (anexar/sobrescribir)\n");
        printf("  6.  Copiar archivo (bloques de 4096 + integridad)\n");
        printf("  7.  Renombrar o mover archivo\n");
        printf("  8.  Cambiar permisos (chmod)\n");
        printf("  9.  Buscar archivos\n");
        printf("  10. Eliminar archivo o directorio\n");
        printf("  11. Volver al menú principal\n");

        if (aviso[0] != '\0') {
            printf("\n\033[1;31m%s\033[0m\n", aviso); /* negrita + rojo */
            aviso[0] = '\0'; /* se consume tras mostrarse */
        }

        {
            int r = leer_entero("\nSeleccione una opción (1-11): ", &op);
            if (r == 0) {
                printf("\n[INFO] Entrada cerrada; se regresa al menú principal.\n");
                return;
            }
            if (r < 0) {
                snprintf(aviso, sizeof(aviso),
                         "[ERROR] Entrada no válida: ingrese SOLO números del 1 al 11.");
                continue;
            }
        }

        switch (op) {
        case 1:  ejecutar_op("LISTAR",              fs_listar);           break;
        case 2:  ejecutar_op("CREAR ARCHIVO",       fs_crear_archivo);    break;
        case 3:  ejecutar_op("CREAR DIRECTORIO",    fs_crear_directorio); break;
        case 4:  ejecutar_op("LEER ARCHIVO",        fs_leer_archivo);     break;
        case 5:  ejecutar_op("EDITAR ARCHIVO",      fs_editar_archivo);   break;
        case 6:  ejecutar_op("COPIAR ARCHIVO",      fs_copiar);           break;
        case 7:  ejecutar_op("RENOMBRAR / MOVER",   fs_renombrar);        break;
        case 8:  ejecutar_op("CAMBIAR PERMISOS",    fs_permisos);         break;
        case 9:  ejecutar_op("BUSCAR ARCHIVOS",     fs_buscar);           break;
        case 10: ejecutar_op("ELIMINAR",            fs_eliminar);         break;
        case 11:
            return;
        default:
            snprintf(aviso, sizeof(aviso),
                     "[ERROR] Opción fuera de rango (1-11). Intente de nuevo.");
            break;
        }
    }
}

/* ==========================================================================
 *  CENTRO DE JUEGOS — control de ciclo de vida de procesos (fork/exec)
 * ========================================================================== */

/*
 * Localiza un ejecutable: si contiene '/', se valida directamente con
 * access(X_OK); si no, se recorre $PATH manualmente (sin system()).
 * Devuelve 0 y copia la ruta absoluta a `salida`.
 */
static int localizar_ejecutable(const char *comando, char *salida, size_t cap)
{
    if (strchr(comando, '/') != NULL) {
        if (access(comando, X_OK) == 0) {
            if (snprintf(salida, cap, "%s", comando) >= (int)cap) {
                errno = ENAMETOOLONG;
                return -1;
            }
            return 0;
        }
        return -1;
    }

    {
        const char *path = getenv("PATH");
        char copia[PATH_MAX];
        char *tok;

        if (path == NULL) {
            errno = ENOENT;
            return -1;
        }
        if (snprintf(copia, sizeof(copia), "%s", path) >= (int)sizeof(copia)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        for (tok = strtok(copia, ":"); tok != NULL; tok = strtok(NULL, ":")) {
            char cand[PATH_MAX];
            const char *dir = (tok[0] == '\0') ? "." : tok;
            if (snprintf(cand, sizeof(cand), "%s/%s", dir, comando)
                    >= (int)sizeof(cand)) {
                continue;
            }
            if (access(cand, X_OK) == 0) {
                if (snprintf(salida, cap, "%s", cand) >= (int)cap) {
                    errno = ENAMETOOLONG;
                    return -1;
                }
                return 0;
            }
        }
    }
    errno = ENOENT;
    return -1;
}

/*
 * Lanza un juego controlando su ciclo de vida completo:
 *   access() -> time() -> tcgetattr() -> fork() -> hijo: execvp()
 *   padre: SIGINT=IGN -> waitpid() -> restaura señales y terminal -> informe.
 * argv[0] debe ser el comando (se reemplaza por su ruta absoluta).
 */
static int lanzar_juego(char *argv[])
{
    char ruta[PATH_MAX];
    char etiqueta[128];
    struct termios original;
    int restaurar_termios = 0;
    time_t inicio, fin;
    pid_t pid;
    int estado = 0;

    snprintf(etiqueta, sizeof(etiqueta), "%s", argv[0]);

    /* 1. Verificación de existencia y permisos de ejecución */
    if (localizar_ejecutable(argv[0], ruta, sizeof(ruta)) != 0) {
        printf("[ERROR] El ejecutable '%s' no existe o no tiene permisos X_OK.\n",
               argv[0]);
        printf("        Verifique la instalación (paquete 'bsdgames').\n");
        registrar("ERROR juego '%s' no encontrado o sin permisos (X_OK)", argv[0]);
        return -1;
    }
    argv[0] = ruta; /* execvp usa la ruta absoluta resuelta */

    /* 2. Registro de hora exacta de inicio */
    inicio = time(NULL);

    /* 3. Estado de la terminal antes de lanzar el juego */
    if (tcgetattr(STDIN_FILENO, &original) == 0) {
        restaurar_termios = 1;
    } else {
        printf("[AVISO] tcgetattr: %s (se continúa igualmente)\n", strerror(errno));
    }

    printf("\n[PROCESO] Lanzando '%s'...\n", etiqueta);
    registrar("JUEGO iniciado '%s'", etiqueta);

    /* 4. Creación de proceso */
    pid = fork();
    if (pid < 0) {
        printf("[ERROR] fork: %s\n", strerror(errno));
        registrar("ERROR fork para '%s': %s", etiqueta, strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* ---------- PROCESO HIJO ---------- */
        signal(SIGINT, SIG_DFL);   /* el juego debe recibir Ctrl+C */
        execvp(argv[0], argv);     /* nunca debe volver si tiene éxito */
        perror("execvp");
        _exit(127);                /* evita duplicar menús / procesos fantasma */
    }

    /* ---------- PROCESO PADRE ---------- */
    signal(SIGINT, SIG_IGN);       /* Ctrl+C no debe matar el menú */

    while (waitpid(pid, &estado, 0) < 0) {
        if (errno != EINTR) {
            printf("[ERROR] waitpid: %s\n", strerror(errno));
            break;
        }
    }

    /* Restauración de señales y configuración original de la terminal */
    signal(SIGINT, SIG_DFL);
    if (restaurar_termios) {
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &original) != 0) {
            printf("[AVISO] tcsetattr: %s\n", strerror(errno));
        }
    }

    fin = time(NULL);

    /* 7. Informe de finalización */
    printf("\n=============== INFORME DEL PROCESO ===============\n");
    printf("Juego            : %s\n", etiqueta);
    printf("PID del hijo     : %ld\n", (long)pid);
    if (WIFEXITED(estado)) {
        printf("Estado de salida : finalizó normalmente (WIFEXITED), código %d\n",
               WEXITSTATUS(estado));
    } else if (WIFSIGNALED(estado)) {
        printf("Estado de salida : terminado por la señal %d (WIFSIGNALED)\n",
               WTERMSIG(estado));
    } else if (WIFSTOPPED(estado)) {
        printf("Estado de salida : detenido por la señal %d (WIFSTOPPED)\n",
               WSTOPSIG(estado));
    } else {
        printf("Estado de salida : 0x%x (indeterminado)\n", (unsigned int)estado);
    }
    printf("Tiempo de juego  : %ld segundo(s)\n", (long)(fin - inicio));
    printf("====================================================\n");

    registrar("JUEGO finalizado '%s' (pid %ld, estado 0x%x, %ld s)",
              etiqueta, (long)pid, (unsigned int)estado, (long)(fin - inicio));
    return 0;
}

/*
 * Ejecuta un juego con la interfaz completa: limpia la consola, muestra el
 * encabezado CENTRO DE JUEGOS > JUEGO, lanza el proceso, presenta el informe
 * y espera con ENTER antes de redibujar el menú.
 */
static void ejecutar_juego(const char *titulo, char *argv[])
{
    encabezado("CENTRO DE JUEGOS", titulo);
    lanzar_juego(argv);
    pausa();
}

/* Submenú del Centro de Juegos (bucle propio). */
static void submenu_juegos(void)
{
    int op = 0;
    char aviso[200] = ""; /* último error: se conserva al redibujar el menú */

    for (;;) {
        encabezado("CENTRO DE JUEGOS", NULL); /* limpia y titula el menú */
        printf("  1. Tetris  (tetris-bsd)\n");
        printf("  2. Snake   (snake)\n");
        printf("  3. Robots  (robots)\n");
        printf("  4. Ejecutar otro binario (ruta o comando)\n");
        printf("  5. Volver al menú principal\n");

        if (aviso[0] != '\0') {
            printf("\n\033[1;31m%s\033[0m\n", aviso); /* negrita + rojo */
            aviso[0] = '\0'; /* se consume tras mostrarse */
        }

        {
            int r = leer_entero("\nSeleccione una opción (1-5): ", &op);
            if (r == 0) {
                printf("\n[INFO] Entrada cerrada; se regresa al menú principal.\n");
                return;
            }
            if (r < 0) {
                snprintf(aviso, sizeof(aviso),
                         "[ERROR] Entrada no válida: ingrese SOLO números del 1 al 5.");
                continue;
            }
        }

        switch (op) {
        case 1: {
            char *argv[] = { (char *)"tetris-bsd", NULL };
            ejecutar_juego("TETRIS", argv);
            break;
        }
        case 2: {
            char *argv[] = { (char *)"snake", NULL };
            ejecutar_juego("SNAKE", argv);
            break;
        }
        case 3: {
            char *argv[] = { (char *)"robots", NULL };
            ejecutar_juego("ROBOTS", argv);
            break;
        }
        case 4: {
            char linea[512];
            char *argv[16];
            int argc = 0;
            char *tok;

            encabezado("CENTRO DE JUEGOS", "OTRO EJECUTABLE");
            printf("Comando (ej.: /usr/games/tetris-bsd o snake): ");
            fflush(stdout);
            if (!leer_texto(linea, sizeof(linea)) || linea[0] == '\0') {
                snprintf(aviso, sizeof(aviso), "[ERROR] Comando vacío.");
                break;
            }
            for (tok = strtok(linea, " \t"); tok != NULL && argc < 15;
                 tok = strtok(NULL, " \t")) {
                argv[argc++] = tok;
            }
            argv[argc] = NULL;
            if (argc == 0) {
                snprintf(aviso, sizeof(aviso), "[ERROR] Comando vacío.");
                break;
            }
            lanzar_juego(argv);
            pausa();
            break;
        }
        case 5:
            return;
        default:
            snprintf(aviso, sizeof(aviso),
                     "[ERROR] Opción fuera de rango (1-5). Intente de nuevo.");
            break;
        }
    }
}

/* ==========================================================================
 *  MENÚ PRINCIPAL
 * ========================================================================== */

/*
 * Menú principal: limpia la consola (ANSI) y dibuja el encabezado del
 * sistema más las opciones. Si hay un aviso pendiente, se muestra debajo
 * para que el error no desaparezca con la limpieza.
 */
static void menu_principal(const char *aviso)
{
    encabezado("MINI SISTEMA OPERATIVO v2.0", NULL);
    printf("  Disco virtual: %s\n\n", g_disco);
    printf("  1. Gestor de Archivos\n");
    printf("  2. Centro de Juegos\n");
    printf("  3. Ver Bitácora del Sistema\n");
    printf("  0. Apagar Sistema\n");

    if (aviso != NULL && aviso[0] != '\0') {
        printf("\n\033[1;31m%s\033[0m\n", aviso); /* negrita + rojo */
    }
}

int main(void)
{
    int op = 0;
    char aviso[200] = ""; /* último error: se conserva al redibujar el menú */

    /* El disco virtual debe existir y estar normalizado antes de operar */
    if (inicializar_disco() != 0) {
        return EXIT_FAILURE;
    }

    registrar("SISTEMA iniciado (disco %s)", g_disco);

    for (;;) {
        int r;

        menu_principal(aviso); /* limpia la pantalla y redibuja el menú */
        aviso[0] = '\0';

        r = leer_entero("\nSeleccione una opción (0-3): ", &op);
        if (r == 0) {
            /* EOF (Ctrl+D): salida limpia */
            limpiar_pantalla();
            printf("\n[SISTEMA] Entrada cerrada. Apagando el MiniSO...\n");
            break;
        }
        if (r < 0) {
            snprintf(aviso, sizeof(aviso),
                     "[ERROR] Entrada no válida: letras o números fuera de rango.\n"
                     "        El buffer de entrada fue limpiado; intente de nuevo.");
            continue;
        }

        switch (op) {
        case 1:
            submenu_archivos(); /* submenú con su propio encabezado */
            break;
        case 2:
            submenu_juegos();   /* submenú con su propio encabezado */
            break;
        case 3:
            encabezado("SISTEMA", "BITÁCORA DEL SISTEMA");
            ver_bitacora();
            pausa();
            break;
        case 0:
            registrar("SISTEMA apagado por el usuario");
            limpiar_pantalla();
            printf("\n[SISTEMA] Apagando el Mini Sistema Operativo. ¡Hasta luego!\n");
            return EXIT_SUCCESS;
        default:
            snprintf(aviso, sizeof(aviso),
                     "[ERROR] Opción inválida '%d'. Use 0, 1, 2 o 3.", op);
            break;
        }
    }

    registrar("SISTEMA finalizado (EOF)");
    return EXIT_SUCCESS;
}
