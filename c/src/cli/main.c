#include "downloader/application.h"
#include "downloader/domain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *program)
{
    printf(
        "Downloader C - núcleo inicial da migração\n\n"
        "Uso:\n"
        "  %s version\n"
        "  %s queue-demo\n\n"
        "A CLI completa (analyze/download/convert) será ligada ao executor de\n"
        "processos quando a camada de infraestrutura for portada.\n",
        program,
        program
    );
}

static int run_queue_demo(void)
{
    DldApplicationState state;
    dld_application_state_init(&state, dld_queue_limits_default());

    DldAppError error;
    dld_app_error_init(&error);

    DldTaskRecord first;
    DldTaskRecord second;
    dld_task_record_init(&first);
    dld_task_record_init(&second);

    /*
     * A demonstração cria modelos temporários e usa a mesma cópia profunda
     * empregada pela aplicação real. Assim o exemplo também exercita ownership.
     */
    DldTaskRecord first_source;
    DldTaskRecord second_source;
    dld_task_record_init(&first_source);
    dld_task_record_init(&second_source);

    first_source.id = malloc(7U);
    second_source.id = malloc(7U);
    if (first_source.id == NULL || second_source.id == NULL) {
        fprintf(stderr, "Falha de memória.\n");
        dld_task_record_clear(&first_source);
        dld_task_record_clear(&second_source);
        dld_application_state_clear(&state);
        dld_app_error_clear(&error);
        return EXIT_FAILURE;
    }

    memcpy(first_source.id, "demo-1", 7U);
    memcpy(second_source.id, "demo-2", 7U);
    first_source.kind = DLD_TASK_CONVERT;
    second_source.kind = DLD_TASK_CONVERT;

    if (!dld_task_record_copy(&first, &first_source) ||
        !dld_task_record_copy(&second, &second_source)) {
        fprintf(stderr, "Falha ao preparar demonstração.\n");
        dld_task_record_clear(&first_source);
        dld_task_record_clear(&second_source);
        dld_task_record_clear(&first);
        dld_task_record_clear(&second);
        dld_application_state_clear(&state);
        dld_app_error_clear(&error);
        return EXIT_FAILURE;
    }

    dld_task_record_clear(&first_source);
    dld_task_record_clear(&second_source);

    DldTaskEvent event;
    if (!dld_application_enqueue(&state, &first, &event, &error) ||
        !dld_application_enqueue(&state, &second, &event, &error)) {
        fprintf(stderr, "Erro: %s\n", error.message != NULL ? error.message : "desconhecido");
        dld_task_record_clear(&first);
        dld_task_record_clear(&second);
        dld_application_state_clear(&state);
        dld_app_error_clear(&error);
        return EXIT_FAILURE;
    }

    DldStartResult started = dld_application_start_next(&state, &event, &error);
    if (started != DLD_START_STARTED) {
        fprintf(stderr, "Não foi possível iniciar a primeira tarefa.\n");
        dld_task_record_clear(&first);
        dld_task_record_clear(&second);
        dld_application_state_clear(&state);
        dld_app_error_clear(&error);
        return EXIT_FAILURE;
    }

    printf("Iniciada: %s (%s)\n", event.task_id, dld_task_status_name(event.status));

    started = dld_application_start_next(&state, &event, &error);
    printf(
        "Segunda conversão: %s\n",
        started == DLD_START_NONE ? "aguardando limite de concorrência" : "estado inesperado"
    );

    dld_task_record_clear(&first);
    dld_task_record_clear(&second);
    dld_application_state_clear(&state);
    dld_app_error_clear(&error);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        print_usage(argv[0]);
        return argc == 1 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (strcmp(argv[1], "version") == 0) {
        puts("downloader-c 0.1.0-port");
        return EXIT_SUCCESS;
    }

    if (strcmp(argv[1], "queue-demo") == 0) {
        return run_queue_demo();
    }

    print_usage(argv[0]);
    return EXIT_FAILURE;
}
