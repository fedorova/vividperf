#ifndef _LIST_H_
#define _LIST_H_

typedef struct node {
    void *data;
    struct node *next;	
} Node;

Node *list_create(void *data);
Node *list_insert_after(Node *node, void *data);
Node *list_insert_beginning(Node *list, void *data);
Node *list_insert_end(Node *list, void *data);
int list_insert(Node **list, void *data);
void list_insert_and_exit_on_error(Node **list, void *data, char *file, int line);
int list_remove(Node *list, Node *node);
int list_foreach(Node *node, int(*func)(void*));

#endif
