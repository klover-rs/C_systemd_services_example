#include <stdio.h>
#include <stdlib.h>
#include <systemd/sd-bus.h>
#include <wchar.h>
#include <stdbool.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

bool EnumerateServiceNames(char*** serviceNames, int* count) {
    sd_bus *bus = NULL;
    sd_bus_message *reply = NULL;
    int r;

    r = sd_bus_open_system(&bus);
    if (r < 0) {
        fprintf(stderr, "failed to connect to bus: %s\n", strerror(-r));
        sd_bus_unref(bus);
        return false;
    }

    r = sd_bus_call_method(
        bus, 
        "org.freedesktop.systemd1",     
        "/org/freedesktop/systemd1",    
        "org.freedesktop.systemd1.Manager", 
        "ListUnits",                     
        NULL,                            
        &reply,
        NULL
    );

    if (r < 0) {
        fprintf(stderr, "Failed to call ListUnits method: %s\n", strerror(-r));
        sd_bus_unref(bus);
        return false;
    }

    r = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "(ssssssouso)");
    if (r < 0) {
        fprintf(stderr, "Failed to enter container: %s\n", strerror(-r));
        sd_bus_message_unref(reply);
        sd_bus_unref(bus);
        return false;
    }

    const char *name;
    const char *description;
    const char *load_state;
    const char *active_state;
    const char *sub_state;
    const char *following;
    uint32_t unit_id;
    const char *object_path;
    const char *job_type;
    const char *job_path;

    int capacity = 10;
    *serviceNames = malloc(capacity * sizeof(char*));
    if (!*serviceNames) {
        fprintf(stderr, "Failed to allocate memory for service names\n");
        sd_bus_message_exit_container(reply);
        sd_bus_message_unref(reply);
        sd_bus_unref(bus);
        return false;
    }

    *count = 0;

    while ((r = sd_bus_message_read(reply, "(ssssssouso)", &name, &description, &load_state, &active_state, &sub_state, &following, &unit_id, &object_path, &job_type, &job_path)) > 0) {
        
        if (strstr(name, ".service") != NULL) {
            if (*count >= capacity) {
                capacity *= 2;
                char **temp = realloc(*serviceNames, capacity * sizeof(char*));
                if (!temp) {
                    fprintf(stderr, "Failed to reallocate memory for service names\n");
                    for (int i = 0; i < *count; i++) {
                        free((*serviceNames)[i]);
                    }
                    sd_bus_message_exit_container(reply);
                    sd_bus_message_unref(reply);
                    sd_bus_unref(bus);
                    return false;
                }
                *serviceNames = temp;
            }
            (*serviceNames)[*count] = strdup(name);
            if (!(*serviceNames)[*count]) {
                fprintf(stderr, "failed to duplicate service name\n");
                for (int i = 0; i < *count; i++) {
                    free((*serviceNames)[i]);
                }
                sd_bus_message_exit_container(reply);
                sd_bus_message_unref(reply);
                sd_bus_unref(bus);
                return false;
            }
            (*count)++;
        }
    }

    if (r < 0) {
        fprintf(stderr, "Failed to read message: %s\n", strerror(-r));
        for (int i = 0; i < *count; i++) {
            free((*serviceNames)[i]);
        }
        free(*serviceNames);
        sd_bus_message_exit_container(reply);
        sd_bus_message_unref(reply);
        sd_bus_unref(bus);
        return false;
    }


    return true;
    
}


#define COMMAND_BUFFER_SIZE 256

int execute_command(const char *command, char *output, size_t output_size) {
    FILE *fp;
    char buffer[COMMAND_BUFFER_SIZE];

    fp = popen(command, "r");
    if (fp == NULL) {
        perror("popen");
        return -1;
    }

    if (fgets(buffer, sizeof(buffer), fp) != NULL) {
        strncpy(output, buffer, output_size - 1);
        output[output_size - 1] = '\0';
    } else {
        output[0] = '\0';
    }

    if (pclose(fp) == -1) {
        perror("pclose");
        return -1;
    }

    return 0;
}


#define MAX_LINE_LENGTH 256

typedef struct {
    char type[MAX_LINE_LENGTH];
    char exec_start[MAX_LINE_LENGTH];
    char description[MAX_LINE_LENGTH];
    char user[MAX_LINE_LENGTH];
} ServiceFileData;

void parse_service_file(const char* file_path, ServiceFileData *data) {
    FILE *file;
    char line[MAX_LINE_LENGTH];

    memset(data, 0, sizeof(ServiceFileData));

    file = fopen(file_path, "r");
    if (file == NULL) {
        perror("fopen");
        printf("file path which causes problems: %s\n", file_path);
        return;
    } 

    while (fgets(line, sizeof(line), file)) {
        char *key;
        char *value;

        line[strcspn(line, "\r\n")] = 0;

        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        key = strtok(line, "=");
        value = strtok(NULL, "=");

        if (value) {
            if (strcmp(key, "Type") == 0) {
                strncpy(data->type, value, sizeof(data->type) - 1);
            } else if (strcmp(key, "ExecStart") == 0) {
                strncpy(data->exec_start, value, sizeof(data->exec_start) - 1);
            } else if (strcmp(key, "Description") == 0) {
                strncpy(data->description, value, sizeof(data->description) - 1);
            } else if (strcmp(key, "User") == 0) {
                strncpy(data->user, value, sizeof(data->user) - 1);
            }
        }
    }
}

typedef struct {
    char service_name[COMMAND_BUFFER_SIZE];
    char fragment_path[COMMAND_BUFFER_SIZE];
    char type[MAX_LINE_LENGTH];
    char exec_start[MAX_LINE_LENGTH];
    char description[MAX_LINE_LENGTH];
    char user[MAX_LINE_LENGTH];
} ThreadData;

void *process_services(void *service_name);

int main() {

    clock_t start_time = clock();
    
    char **serviceNames = NULL;
    int count = 0;

    char command[COMMAND_BUFFER_SIZE];
    char output[COMMAND_BUFFER_SIZE];
    char *path_start;


    if (EnumerateServiceNames(&serviceNames, &count)) {

        pthread_t threads[count];
        ThreadData *thread_data_array = malloc(count * sizeof(ThreadData));

        for (int i = 0; i < count; i++) {

            char *service_name_copy = strdup(serviceNames[i]);

            if (service_name_copy == NULL) {
                perror("strdup");
                exit(EXIT_FAILURE);
            }

            if (pthread_create(&threads[i], NULL, process_services, (void *)service_name_copy) != 0) {
                perror("pthread_create");
                free(service_name_copy);
                exit(EXIT_FAILURE);
            }

            


        }
    
        for (int i = 0; i < count; i++) {
            void *result;
            if (pthread_join(threads[i], &result) != 0) {
                perror("pthread_join");
                exit(EXIT_FAILURE);
            }

            thread_data_array[i] = *(ThreadData *)result;
            free(result);
        }

        for (int i = 0; i < count; i++) {
            free(serviceNames[i]);
        }

        free(serviceNames); 

        for (int i = 0; i < count; i++) {
            printf("Service Name: %s\n", thread_data_array[i].service_name);
            printf("Fragment Path: %s\n", thread_data_array[i].fragment_path);
            printf("Type: %s\n", thread_data_array[i].type);
            printf("ExecStart: %s\n", thread_data_array[i].exec_start);
            printf("Description: %s\n", thread_data_array[i].description);
            printf("User: %s\n\n", thread_data_array[i].user);
        }

        free(thread_data_array);
        
    } else {
        fprintf(stderr, "Failed to enumerate service names\n");
    }

    clock_t end_time = clock();

    double elapsed_time = (double)(end_time - start_time) / CLOCKS_PER_SEC;
    printf("Elapsed time: %.2f seconds\n", elapsed_time);
    return 0;
}

void *process_services(void *service_name) {
    char command[COMMAND_BUFFER_SIZE];
    char output[COMMAND_BUFFER_SIZE];
    char *path_start;
    ThreadData *thread_data = malloc(sizeof(ThreadData));

    if (thread_data == NULL) {
        perror("malloc");
        pthread_exit(NULL);
    }

    const char *service_name_char = (const char *)service_name;

    snprintf(thread_data->service_name, sizeof(thread_data->service_name), "%s", service_name_char);

    snprintf(command, sizeof(command), "systemctl show -p FragmentPath %s", service_name_char);
    if (execute_command(command, output, sizeof(output)) != 0) {
        fprintf(stderr, "Failed to execute command.\n");
        free(thread_data);
        return NULL;
    }

    if ((path_start = strstr(output, "FragmentPath=")) != NULL) {
        path_start += strlen("FragmentPath=");
        path_start[strcspn(path_start, "\r\n")] = '\0';
        snprintf(thread_data->fragment_path, sizeof(thread_data->fragment_path), "%s", path_start);

        ServiceFileData data;

        parse_service_file(path_start, &data);

        if (strcmp(data.type, "") == 0) {
            snprintf(thread_data->type, sizeof(thread_data->type), "Not specified");
        } else {
            snprintf(thread_data->type, sizeof(thread_data->type), "%s", data.type);
        }

        if (strcmp(data.description, "") == 0) {
            snprintf(thread_data->description, sizeof(thread_data->description), "Not specified");
        } else {
            snprintf(thread_data->description, sizeof(thread_data->description), "%s", data.description);
        }

        if (strcmp(data.exec_start, "") == 0) {
            snprintf(thread_data->exec_start, sizeof(thread_data->exec_start), "Not specified");
        } else {
            snprintf(thread_data->exec_start, sizeof(thread_data->exec_start), "%s", data.exec_start);
        }

        if (strcmp(data.user, "") == 0) {
            snprintf(thread_data->user, sizeof(thread_data->user), "Not specified");
        } else {
            snprintf(thread_data->user, sizeof(thread_data->user), "%s", data.user);
        }

    } else {
        snprintf(thread_data->fragment_path, sizeof(thread_data->fragment_path), "Service file not found");
    }

    free(service_name);

    return (void *)thread_data;
}