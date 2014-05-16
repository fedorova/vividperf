/*
 * Simple linked list. 
 * 
 * Adapted from http://en.literateprograms.org/Singly_linked_list_(C)
 */

#include <stdlib.h>
#include <stdio.h>
#include "list.h"

Node *list_create(void *data)
{
    Node *node;
    if(!(node=malloc(sizeof(Node)))) 
	return NULL;
    
    node->data = data;
    node->next = NULL;
    return node;
}

void list_insert_and_exit_on_error(Node **list, void *data, char *file, int line)
{
    int ret = list_insert(list, data);
    
    if(ret)
    {
	fprintf(stderr, 
		"Couldn't insert data into list. File: %s, line: %d\n", 
		file, line);
	exit(-1);
    }
}


int list_insert(Node **list, void *data)
{
    *list = list_insert_end(*list, data);
    if(*list == NULL)
	return -1;
    else
	return 0;
}
   

Node *list_insert_after(Node *node, void *data)
{
    Node *newnode;
    newnode = list_create(data);

    if(newnode == NULL)
	return NULL;

    newnode->next = node->next;
    node->next = newnode;
    return newnode;
}


Node *list_insert_beginning(Node *list, void *data)
{
    Node *newnode;
    newnode = list_create(data);
      
    if(newnode == NULL)
	return NULL;

    newnode->next = list;
    return newnode;
}

Node *list_insert_end(Node *list, void *data)
{
    if(list == NULL)
	return list_insert_beginning(list, data);


    Node *newnode;
    newnode = list_create(data);
      
    if(newnode == NULL)
	return NULL;

    Node *curnode = list;

    while(curnode)
    {
	if(curnode->next == NULL)
	{
	    curnode->next = newnode;
	    newnode->next = NULL;
	    break;
	}
	curnode = curnode->next;
    }
    return list;
}


int list_remove(Node *list, Node *node)
{
    while(list->next && list->next != node) 
	list = list->next;

    if(list->next) 
    {
	list->next = node->next;
	free(node);
	return 0;		
    } 
    else 
	return -1;
}

int list_foreach(Node *node, int(*func)(void*))
{
    while(node) 
    {
	if(func(node->data) != 0) 
	    return -1;
	node = node->next;
    }
    return 0;
}

