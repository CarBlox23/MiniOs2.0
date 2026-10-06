#include <stdio.h>
#include <stdlib.h>

void mostrarMenu() {
    printf("\n==========================================\n");
    printf("   MINI SISTEMA OPERATIVO - MENÚ CLI\n");
    printf("==========================================\n");
    printf("GESTIÓN DE MEMORIA SECUNDARIA (CRUD):\n");
    printf("  1. Create (Ingresar registros)\n");
    printf("  2. Read   (Leer/Extraer registros)\n");
    printf("  3. Update (Modificar registros)\n");
    printf("  4. Delete (Remover registros)\n");
    printf("GESTIÓN DE PROCESOS:\n");
    printf("  5. Iniciar Reto BSDGames\n");
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
            while (getchar() != '\n');
            continue;
        }

        printf("\n------------------------------------------\n");
        
        
        switch (opcion) {
            case 1:
                printf("[SISTEMA] Ejecutando Create: Creando registro...\n");
                break;
            case 2:
                printf("[SISTEMA] Ejecutando Read: Leyendo registros del disco...\n");
                break;
            case 3:
                printf("[SISTEMA] Ejecutando Update: Actualizando registro...\n");
                break;
            case 4:
                printf("[SISTEMA] Ejecutando Delete: Eliminando registro...\n");
                break;
            case 5:
                printf("[SISTEMA] Ejecutando Reto BSDGames: Invocando juego...\n");
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
