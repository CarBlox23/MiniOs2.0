#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARCHIVO_DISCO "disco.dat"

// Estructura de datos para simular un registro guardado en la Memoria Secundaria
typedef struct {
    int id;
    char nombre[50];
    int tamano_kb;
} Registro;

// Prototipos de funciones
void crearRegistro();
void leerRegistros();
void actualizarRegistro();
void eliminarRegistro();
void ejecutarBSDGames();
void mostrarMenu();

void mostrarMenu() {
    printf("\n==========================================\n");
    printf("   MINI SISTEMA OPERATIVO - MENÚ CLI\n");
    printf("==========================================\n");
    printf("GESTIÓN DE MEMORIA SECUNDARIA (CRUD):\n");
    printf("  1. Create (Ingresar registro al disco)\n");
    printf("  2. Read   (Leer registros del disco)\n");
    printf("  3. Update (Modificar registro por ID)\n");
    printf("  4. Delete (Remover registro por ID)\n");
    printf("GESTIÓN DE PROCESOS:\n");
    printf("  5. Iniciar Reto BSDGames (Tetris/Snake)\n");
    printf("CONTROL DEL SISTEMA:\n");
    printf("  6. Apagar Sistema\n");
    printf("==========================================\n");
    printf("Seleccione una opción (1-6): ");
}

int main() {
    int opcion = 0;

    while (1) {
        mostrarMenu();

        if (scanf("%d", &opcion) != 1) {
            printf("\n[ERROR] Entrada no válida. Por favor ingrese un número.\n");
            while (getchar() != '\n'); // Limpiar buffer de entrada
            continue;
        }

        printf("\n------------------------------------------\n");

        switch (opcion) {
            case 1:
                crearRegistro();
                break;
            case 2:
                leerRegistros();
                break;
            case 3:
                actualizarRegistro();
                break;
            case 4:
                eliminarRegistro();
                break;
            case 5:
                ejecutarBSDGames();
                break;
            case 6:
                printf("[SISTEMA] Apagando el Mini Sistema Operativo. ¡Hasta luego!\n");
                printf("------------------------------------------\n");
                return 0;
            default:
                printf("[ADVERTENCIA] Opción inválida. Intente con un número del 1 al 6.\n");
                break;
        }

        printf("------------------------------------------\n");
    }

    return 0;
}

// 1. CREATE: Guarda un nuevo registro en el disco (archivo binario)
void crearRegistro() {
    FILE *archivo = fopen(ARCHIVO_DISCO, "ab"); // Modo append binario
    if (archivo == NULL) {
        printf("[ERROR] No se pudo acceder a la memoria secundaria (disco.dat).\n");
        return;
    }

    Registro reg;
    printf("--- CREAR REGISTRO EN DISCO ---\n");
    printf("Ingrese ID único del registro: ");
    scanf("%d", &reg.id);
    getchar(); // Limpiar el salto de línea

    printf("Ingrese nombre del archivo/proceso: ");
    fgets(reg.nombre, sizeof(reg.nombre), stdin);
    reg.nombre[strcspn(reg.nombre, "\n")] = 0; // Quitar el enter final

    printf("Ingrese tamaño ocupado en memoria (KB): ");
    scanf("%d", &reg.tamano_kb);

    fwrite(&reg, sizeof(Registro), 1, archivo);
    fclose(archivo);

    printf("\n[ÉXITO] Registro guardado exitosamente en 'disco.dat'.\n");
}

// 2. READ: Extrae y lista todos los registros grabados en el disco
void leerRegistros() {
    FILE *archivo = fopen(ARCHIVO_DISCO, "rb"); // Modo lectura binaria
    if (archivo == NULL) {
        printf("[INFO] La memoria secundaria está vacía o no existe aún.\n");
        return;
    }

    Registro reg;
    int contador = 0;

    printf("--- LECTURA DE MEMORIA SECUNDARIA (disco.dat) ---\n");
    printf("%-10s %-30s %-15s\n", "ID", "NOMBRE", "TAMAÑO (KB)");
    printf("---------------------------------------------------\n");

    while (fread(&reg, sizeof(Registro), 1, archivo) == 1) {
        printf("%-10d %-30s %-15d\n", reg.id, reg.nombre, reg.tamano_kb);
        contador++;
    }

    fclose(archivo);

    if (contador == 0) {
        printf("No se encontraron registros en la memoria.\n");
    } else {
        printf("---------------------------------------------------\n");
        printf("Total de registros cargados: %d\n", contador);
    }
}

// 3. UPDATE: Localiza un registro por su ID y actualiza sus atributos
void actualizarRegistro() {
    FILE *archivo = fopen(ARCHIVO_DISCO, "rb");
    if (archivo == NULL) {
        printf("[INFO] No existe memoria secundaria cargada.\n");
        return;
    }

    FILE *temporal = fopen("temp.dat", "wb");
    if (temporal == NULL) {
        printf("[ERROR] Error al procesar memoria temporal.\n");
        fclose(archivo);
        return;
    }

    int idBuscado, encontrado = 0;
    printf("--- MODIFICAR REGISTRO ---\n");
    printf("Ingrese el ID del registro que desea actualizar: ");
    scanf("%d", &idBuscado);

    Registro reg;
    while (fread(&reg, sizeof(Registro), 1, archivo) == 1) {
        if (reg.id == idBuscado) {
            encontrado = 1;
            printf("\n[REGISTRO ENCONTRADO] Nombre actual: %s | Tamaño: %d KB\n", reg.nombre, reg.tamano_kb);
            getchar(); // Limpiar buffer

            printf("Ingrese el nuevo nombre: ");
            fgets(reg.nombre, sizeof(reg.nombre), stdin);
            reg.nombre[strcspn(reg.nombre, "\n")] = 0;

            printf("Ingrese el nuevo tamaño (KB): ");
            scanf("%d", &reg.tamano_kb);
        }
        fwrite(&reg, sizeof(Registro), 1, temporal);
    }

    fclose(archivo);
    fclose(temporal);

    remove(ARCHIVO_DISCO);
    rename("temp.dat", ARCHIVO_DISCO);

    if (encontrado) {
        printf("\n[ÉXITO] El registro ID %d ha sido actualizado.\n", idBuscado);
    } else {
        printf("\n[ADVERTENCIA] No se encontró ningún registro con ID %d.\n", idBuscado);
    }
}

// 4. DELETE: Elimina un registro del disco sin corromper el resto de los datos
void eliminarRegistro() {
    FILE *archivo = fopen(ARCHIVO_DISCO, "rb");
    if (archivo == NULL) {
        printf("[INFO] La memoria secundaria está vacía.\n");
        return;
    }

    FILE *temporal = fopen("temp.dat", "wb");
    if (temporal == NULL) {
        printf("[ERROR] Error al procesar memoria temporal.\n");
        fclose(archivo);
        return;
    }

    int idEliminar, borrado = 0;
    printf("--- ELIMINAR REGISTRO ---\n");
    printf("Ingrese el ID del registro a eliminar: ");
    scanf("%d", &idEliminar);

    Registro reg;
    while (fread(&reg, sizeof(Registro), 1, archivo) == 1) {
        if (reg.id == idEliminar) {
            borrado = 1; // Se omite la escritura para eliminarlo
        } else {
            fwrite(&reg, sizeof(Registro), 1, temporal);
        }
    }

    fclose(archivo);
    fclose(temporal);

    remove(ARCHIVO_DISCO);
    rename("temp.dat", ARCHIVO_DISCO);

    if (borrado) {
        printf("\n[ÉXITO] Registro ID %d eliminado correctamente sin afectar la memoria.\n", idEliminar);
    } else {
        printf("\n[ADVERTENCIA] Registro ID %d no encontrado.\n", idEliminar);
    }
}

// 5. BSDGAMES: Invoca y ejecuta un juego clásico de terminal (Tetris / Snake)
void ejecutarBSDGames() {
    printf("--- GESTIÓN DE PROCESOS: RETO BSDGAMES ---\n");
    printf("Lanzando proceso 'tetris-bsd' en la terminal...\n");
    printf("Presione ENTER para iniciar el juego (para salir del juego use 'q')...\n");
    getchar();
    getchar();

    // Invoca el proceso del sistema operativo
    int estado = system("tetris-bsd");

    if (estado != 0) {
        printf("\n[AVISO] No se pudo ejecutar 'tetris-bsd'. Probando juego alternativo 'snake'...\n");
        system("snake");
    }

    printf("\n[PROCESO FINALIZADO] Retornando al control del Mini Sistema Operativo...\n");
}
