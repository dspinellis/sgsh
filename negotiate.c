/* Placeholder: LICENSE. */

#include <assert.h> /* assert() */
#include <err.h> /* err() */
#include <errno.h> /* EAGAIN */
#include <stdio.h> /* fprintf() in DPRINTF() */
#include <stdlib.h> /* getenv(), errno */
#include <string.h> /* memcpy() */
#include <unistd.h> /* getpid(), getpagesize(), 
			STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO */
#include "sgsh-negotiate.h" /* sgsh_negotiate(), sgsh_run() */
#include "sgsh.h" /* DPRINTF() */

#define OP_SUCCESS 0
#define OP_ERROR 1
#define OP_QUIT 2
#define OP_EXISTS 3
#define OP_CREATE 4
#define OP_NOOP 5

/* TODO: message block size vs page size check before write. */

/* Identifies the node and node's fd that sent the message block. */
struct dispatcher_node {
	int index;
	int fd_direction;
};

/* Models an I/O connection between tools on an sgsh graph. */
struct sgsh_edge {
	int from; /* Index of node on the graph where data comes from (out). */
	int to; /* Index of node on the graph that receives the data (in). */
};

/* Each tool that participates in an sgsh graph is modelled as follows. */
struct sgsh_node {
        pid_t pid; 
        char name[100];
        int requires_channels; /* Input channels it can take. */
        int provides_channels; /* Output channels it can provide. */
	int sgsh_in;   /* Takes input from other tool(s) on sgsh graph. */
	int sgsh_out;  /* Provides output to other tool(s) on sgsh graph. */
};

/* The message block structure that provides the vehicle for negotiation. */
struct sgsh_negotiation {
	double version; /* Protocol version. */
        struct sgsh_node *node_array;
        int n_nodes;
	struct sgsh_edge *edge_array;
        int n_edges;
	pid_t initiator_pid;
        int state_flag;
        int serial_no;
	struct dispatcher_node origin;
	size_t total_size; /* Compact allocation that includes sgsh_node
				memory allocation. */
};

/**
 * Memory organisation of message block.
 * Because message block will be passed around process address spaces, 
 * any pointers should point to slots internal to the message block. 
 * Therefore a message block allocation (structure sgsh_negotiation) 
 * contains related structure instances such as the node and edge 
 * arrays organised as follows:
 *
 * struct sgsh_negotiation  
 * --                       
 * struct sgsh_node (array) 
 * ... (x n_nodes)          
 * --                       
 * struct sgsh_edge (array) 
 * ... (x n_edges)          
 */

static struct sgsh_negotiation *chosen_mb; /* Our king message block. */
static struct sgsh_node self_node; /* The sgsh node that models this tool. */
static struct dispatcher_node self_dispatcher; /* Dispatch info for this tool.*/

/* Reallocate array to edge pointers. */
static int 
reallocate_edge_pointer_array(struct sgsh_edge ***edge_array, 
						int n_elements)
{
	void **p = realloc(*edge_array, sizeof(struct sgsh_edge *) * n_elements);
	if (!p) {
		err(1, "Memory reallocation for edge failed.\n");
		return OP_ERROR;
	} else
		*edge_array = (struct sgsh_edge **)p;
	return OP_SUCCESS;
}

/** 
 * Lookup this tool's edges and store pointers to them in order
 * to allow the allocation of connections.
 */
static int 
lookup_sgsh_edges(struct sgsh_edge **edges_incoming, 
				int *n_edges_incoming, 
				struct sgsh_edge **edges_outgoing,
				int *n_edges_outgoing)
{
	int self_index = self_dispatcher.index; /* This tool's node index. */
	int n_edges = chosen_mb->n_edges;
	int i;
	for (i = 0; i < n_edges; i++) {
		struct sgsh_edge *edge = &chosen_mb->edge_array[i];
		if (edge->from == self_index) {
			(*n_edges_outgoing)++;
			if (reallocate_edge_pointer_array(&edges_outgoing, 
					*n_edges_outgoing) == OP_ERROR)
				return OP_ERROR;
		}
		if (edge->to == self_index) {
			(*n_edges_incoming)++;
			if (reallocate_edge_pointer_array(&edges_incoming, 
					*n_edges_incoming) == OP_ERROR)
				return OP_ERROR;
		}
	}
	return OP_SUCCESS;
}

/**
 * Look for a solution to the graph's path requirements for this tool.
 * Allocate pipes to connect this node to other nodes on the graph.
 */
static int
allocate_io_connections(int **input_fds, int *n_input_fds, int **output_fds, 
							int *n_output_fds)
{
	struct sgsh_edge **self_edges_incoming = NULL;
	int n_self_edges_incoming = 0;
	struct sgsh_edge **self_edges_outgoing = NULL;
	int n_self_edges_outgoing = 0;

	lookup_sgsh_edges(self_edges_incoming, &n_self_edges_incoming, 
				self_edges_outgoing, &n_self_edges_outgoing);
	if ((n_self_edges_incoming != self_node.requires_channels) ||
	    (n_self_edges_outgoing != self_node.provides_channels)) {
		err(1, "Failed to satisfy requirements for tool %s, pid %d: requires %d and gets %d, provides %d and is offered %d.\n", self_node.name,
		self_node.pid, self_node.requires_channels, 
		n_self_edges_incoming, self_node.provides_channels, 
		n_self_edges_outgoing);
		return OP_ERROR;
	}
	/* allocate_fds(); */
	return OP_SUCCESS;
}

/** 
 * Copy the dispatcher static object that identifies the node
 * in the message block node array and shows the write point of
 * the send operation. This is a deep copy for simplicity. 
 */
static void
set_dispatcher() {
	chosen_mb->origin.index = self_dispatcher.index;
	assert(self_dispatcher.index >= 0); /* Node is added to the graph. */
	chosen_mb->origin.fd_direction = self_dispatcher.fd_direction;
}

/* Write message block to buffer. */
static int
write_mb(char *buf, int buf_size)
{
	if (chosen_mb->total_size > buf_size) return OP_ERROR;
	set_dispatcher();
	memcpy(buf, chosen_mb, chosen_mb->total_size);
	write(self_dispatcher.fd_direction, buf, chosen_mb->total_size);
	DPRINTF("Ship message block to next node in graph from file descriptor: %s.\n", (self_dispatcher.fd_direction) ? "stdout" : "stdin");
	return OP_SUCCESS;
}

/* Check whether negotiation phase should end. */
static void
check_negotiation_round(int *negotiation_round, int updated_mb_serial_no)
{
	if (self_node.pid == chosen_mb->initiator_pid) { /* Round end? */
		(*negotiation_round)++;
		if ((*negotiation_round == 3) && (!updated_mb_serial_no)) {
			chosen_mb->state_flag = PROT_STATE_NEGOTIATION_END;
			chosen_mb->serial_no++;
			DPRINTF("Negotiation protocol state change: end of negotiation phase.\n");
		}
	}
}

/* Reallocate message block to fit new node coming in. */
static int
realloc_mb_new_node()
{
	int new_size = chosen_mb->total_size + sizeof(struct sgsh_node);
	int old_size = chosen_mb->total_size;

	/* Make room for new node. Copy-move edge array. */
	int n_edges = chosen_mb->n_edges;
	int edges_size = sizeof(struct sgsh_edge) * chosen_mb->n_edges;
	struct sgsh_edge edge_array_store[n_edges];
	memcpy(edge_array_store, chosen_mb->edge_array, edges_size);

	void *p = realloc(chosen_mb, new_size);
	if (!p) {
		err(1, "Message block reallocation for adding a new node \
			failed.\n");
		return OP_ERROR;
	} else {
		chosen_mb = (struct sgsh_negotiation *)p;
		chosen_mb->total_size = new_size;

		/* Node instances go after the message block instance. */
		chosen_mb->node_array = (struct sgsh_node *)&chosen_mb[1];
		DPRINTF("Reallocated memory (%d -> %d) to message block to fit new node.\n", old_size, new_size);

		/* Edge instances go after the node instances. */
		chosen_mb->edge_array = (struct sgsh_edge *)&chosen_mb->node_array[chosen_mb->n_nodes + 1]; /* New node has not been added yet. */
		memcpy(chosen_mb->edge_array, edge_array_store, edges_size);
	}
	return OP_SUCCESS;
}

/* Lookup an edge in the sgsh graph. */
static int
lookup_sgsh_edge(struct sgsh_edge *e)
{
	int i;
	for (i = 0; i < chosen_mb->n_edges; i++) {
		if ((chosen_mb->edge_array[i].from == e->from) &&
		    (chosen_mb->edge_array[i].to == e->to))
			return OP_EXISTS;
	}
	return OP_CREATE;
}

/** 
 * Fill edge depending on input/output fd information 
 * passed by sender and found in receiver (this tool or self).
 */
static int
fill_sgsh_edge(struct sgsh_edge *e)
{
	int i;
	int n_nodes = chosen_mb->n_nodes;
	for (i = 0; n_nodes; i++) /* Check dispatcher node exists. */
		if (i == chosen_mb->origin.index) break;
	if (i == n_nodes) {
		err(1, "Dispatcher node with index position %d not present in graph.\n", chosen_mb->origin.index);
		return OP_ERROR;
	}
	if (chosen_mb->origin.fd_direction == STDIN_FILENO) {
	/** 
         * MB sent from stdin, so dispatcher is the destination of the edge.
	 * Self should be sgsh-active on output side. Self's current fd is stdin
	 * if self is sgsh-active on input side or output side otherwise. 
	 * Self (the recipient) is the source of the edge. 
         */
		e->to = chosen_mb->origin.index; 
		assert(self_node.sgsh_out); 
		assert((self_node.sgsh_in && 
			self_dispatcher.fd_direction == STDIN_FILENO) ||
			self_dispatcher.fd_direction == STDOUT_FILENO);
		e->from = self_dispatcher.index; 
	} else if (chosen_mb->origin.fd_direction == STDOUT_FILENO) { 
		/* Similarly. */
		e->from = chosen_mb->origin.index;
		assert(self_node.sgsh_in);
		assert((self_node.sgsh_out && 
			self_dispatcher.fd_direction == STDOUT_FILENO) ||
			self_dispatcher.fd_direction == STDIN_FILENO);
		e->to = self_dispatcher.index;
	}
	return OP_SUCCESS;
}

/* Reallocate message block to fit new edge coming in. */
static int
realloc_mb_new_edge()
{
	int new_size = chosen_mb->total_size + sizeof(struct sgsh_edge);
	int old_size = chosen_mb->total_size;
	void *p = realloc(chosen_mb, new_size);
	if (!p) {
		err(1, "Message block reallocation for adding a new edge failed.\n");
		return OP_ERROR;
	} else {
		chosen_mb = (struct sgsh_negotiation *)p;
		chosen_mb->total_size = new_size;

		/* Node instances go after the message block instance. */
		chosen_mb->node_array = (struct sgsh_node *)&chosen_mb[1]; 

		/* Edge instances go after the node instances. */
		chosen_mb->edge_array = (struct sgsh_edge *)&chosen_mb->node_array[chosen_mb->n_nodes];

		DPRINTF("Reallocated memory (%d -> %d) to message block to fit new edge.\n", old_size, new_size);
	}
	return OP_SUCCESS;
}

/* Try to add a newly occured edge in the sgsh graph. */
static int
try_add_sgsh_edge()
{
	if (chosen_mb->origin.index >= 0) { /* If MB not created just now: */
		struct sgsh_edge new_edge;
		fill_sgsh_edge(&new_edge);
		if (lookup_sgsh_edge(&new_edge) == OP_CREATE) {
			int n_edges = chosen_mb->n_edges;
			if (realloc_mb_new_edge() == OP_ERROR) return OP_ERROR;
			memcpy(&chosen_mb->edge_array[n_edges], &new_edge, 
						sizeof(struct sgsh_edge));
			DPRINTF("Added edge (%d -> %d) in sgsh graph.\n",
					new_edge.from, new_edge.to);
			chosen_mb->n_edges++;
			DPRINTF("Sgsh graph now has %d edges.\n", 
							chosen_mb->n_edges);
			chosen_mb->serial_no++;
			return OP_SUCCESS;
		}
		return OP_EXISTS;
	}
	return OP_NOOP;
}

/** 
 * Add node to message block. Copy the node using offset-based
 * calculation from the start of the array of nodes.
 */
static int
try_add_sgsh_node()
{
	int n_nodes = chosen_mb->n_nodes;
	int i;
	for (i = 0; i < n_nodes; i++)
		if (chosen_mb->node_array[i].pid == self_node.pid) break;
	if (i == n_nodes) {
		if (realloc_mb_new_node() == OP_ERROR) return OP_ERROR;
		memcpy(&chosen_mb->node_array[n_nodes], &self_node, 
					sizeof(struct sgsh_node));
		self_dispatcher.index = n_nodes;
		DPRINTF("Added node %s indexed in position %d in sgsh graph.\n",
					self_node.name, self_dispatcher.index);
		chosen_mb->n_nodes++;
		DPRINTF("Sgsh graph now has %d nodes.\n", chosen_mb->n_nodes);
		chosen_mb->serial_no++;
		return OP_SUCCESS;
	}
	self_dispatcher.index = i;
	return OP_EXISTS;
}

/* A constructor-like function for struct sgsh_node. */
static void
fill_sgsh_node(const char *tool_name, pid_t pid, int requires_channels, 
						int provides_channels)
{
	self_node.pid = pid;
	memcpy(self_node.name, tool_name, strlen(tool_name) + 1);
	self_node.requires_channels = requires_channels;
	self_node.provides_channels = provides_channels;
	DPRINTF("Sgsh node for tool %s with pid %d created.\n", tool_name, pid);
}

/** 
 * Check if the arrived message block preexists our chosen one
 * and substitute the chosen if so.
 * If the arrived message block is younger discard it and don't
 * forward it.
 * If the arrived is the chosen, try to add the edge.
 */
static int
compete_message_block(struct sgsh_negotiation *fresh_mb, 
		int *should_transmit_mb, int *updated_mb_serial_no)
{
        *should_transmit_mb = 1; /* Default value. */
	*updated_mb_serial_no = 0; /* Default value. */
        if (fresh_mb->initiator_pid < chosen_mb->initiator_pid) { /* New chosen! .*/
		free(chosen_mb); /* Heil compact allocation. */
		chosen_mb = fresh_mb;
                if (try_add_sgsh_node() == OP_ERROR)
			return OP_ERROR; /* Double realloc: one for node, */
                if (try_add_sgsh_edge() == OP_ERROR)
			return OP_ERROR; /* one for edge. */
		*updated_mb_serial_no = 1; /*Substituting chosen_mb is an update.*/
        } else if (fresh_mb->initiator_pid > chosen_mb->initiator_pid) {
		free(fresh_mb); /* Discard MB just read. */
                *should_transmit_mb = 0;
	} else {
		if (fresh_mb->serial_no > chosen_mb->serial_no) {
			*updated_mb_serial_no = 1;
			free(chosen_mb);
			chosen_mb = fresh_mb;
		} else 
			free(fresh_mb);
                if (try_add_sgsh_edge() == OP_ERROR) return OP_ERROR;
	}
	return OP_SUCCESS;
}

/** 
 * Point next write operation to the correct file descriptor: stdin or stdout.
 * If only one is active, stay with that one.
 */
static void
point_io_direction(int current_direction)
{
	if ((current_direction == STDIN_FILENO) && (self_node.sgsh_out))
			self_dispatcher.fd_direction = STDOUT_FILENO;
	else if ((current_direction == STDOUT_FILENO) && (self_node.sgsh_in))
			self_dispatcher.fd_direction = STDIN_FILENO;
}

/* Allocate memory for message_block and copy from buffer. */
static int
alloc_copy_mb(struct sgsh_negotiation *mb, char *buf, int bytes_read, 
							int buf_size)
{
	if (bytes_read != mb->total_size) {
		err(1, "Read %d bytes of message block, expected to read %d.",
			bytes_read, (int)mb->total_size);
		return OP_ERROR;
	}
	if (bytes_read > buf_size) {
		err(1, "Read %d bytes of message block, but buffer can hold up to %d.", bytes_read, buf_size);
		return OP_ERROR;
	}
	mb = (struct sgsh_negotiation *)malloc(bytes_read);
	memcpy(mb, buf, bytes_read);
	return OP_SUCCESS;
}

/** 
 * The actual call to read in the message block.
 * If the call does not succeed or does not signal retry we have
 * to quit operation.
 */
static int
call_read(int fd, char *buf, int buf_size, 
				int *fd_side, /* Mark (input or output) fd. */
				int *bytes_read, 
				int *error_code)
{
	*error_code = 0;
	*fd_side = 0;
	DPRINTF("Try read from %s.\n", (fd) ? "stdout" : "stdin");
	if ((*bytes_read = read(fd, buf, buf_size)) == -1)
		*error_code = -errno;
	if ((*error_code == 0) || (*error_code != -EAGAIN)) {
		*fd_side = 1; /* Mark the side where input is coming from. */
		return OP_QUIT;
	}
	return OP_SUCCESS;
}

/**
 * Read in circulated message block from either direction,
 * that is, input or output side. This capability
 * relies on an extension to a standard shell implementation,
 * e.g., bash, that allows reading and writing to both sides
 * for the negotiation phase. 
 * Set I/O to non-blocking in order to be able to retry on both
 * sides.
 * Returns the fd to write the message block if need be transmitted.
 */
static int
try_read_message_block(char *buf, int buf_size,
					struct sgsh_negotiation *fresh_mb)
{
	int bytes_read;
	int error_code = -EAGAIN;
	int  stdin_side = 0;
	int stdout_size = 0;
	while (error_code == -EAGAIN) { /* Try read from stdin, then stdout. */
		if ((call_read(STDIN_FILENO, buf, buf_size, &stdin_side, 
					&bytes_read, &error_code) == OP_QUIT) ||
		    (call_read(STDOUT_FILENO, buf, buf_size, &stdout_size, 
					&bytes_read, &error_code) == OP_QUIT))
			break;
	}
	if (bytes_read == -1) {  /* Read failed. */
	 	err(1, "Reading from ");
		(stdin_side) ? err(1, "stdin ") : err(1, "stdout ");
		err(1, "file descriptor failed with error code %d.\n", 
						error_code);
		return error_code;
	} else {  /* Read succeeded. */
		if (alloc_copy_mb(fresh_mb, buf, bytes_read, buf_size) == 
		    OP_ERROR) return OP_ERROR;
		point_io_direction(stdin_side);
		DPRINTF("Read succeeded: %d bytes read from %s.\n", bytes_read,
			(self_dispatcher.fd_direction) ? "stdout" : "stdin");
	}
	return OP_SUCCESS;
}

/* Construct a message block to use as a vehicle for the negotiation phase. */
static int
construct_message_block(pid_t self_pid)
{
	int memory_allocation_size = sizeof(struct sgsh_negotiation);
	chosen_mb = (struct sgsh_negotiation *)malloc(
				memory_allocation_size);
	if (!chosen_mb) {
		err(1, "Memory allocation of message block failed.");
		return OP_ERROR;
	}
	chosen_mb->version = 1.0;
	chosen_mb->node_array = NULL;
	chosen_mb->n_nodes = 0;
	chosen_mb->initiator_pid = self_pid;
	chosen_mb->state_flag = PROT_STATE_NEGOTIATION;
	chosen_mb->serial_no = 0;
	chosen_mb->origin.index = -1;
	chosen_mb->origin.fd_direction = -1;
	chosen_mb->total_size = memory_allocation_size;
	DPRINTF("Message block created by pid %d.\n", (int)self_pid);
	return OP_SUCCESS;
}

/* Get environment variable env_var. */
static int
get_env_var(const char *env_var,int *value)
{
	char *string_value = getenv(env_var);
	if (!string_value) {
		err(1, "Getting environment variable %s failed.\n", env_var);
		return OP_ERROR;
	} else
		DPRINTF("getenv() returned string value %s.\n", string_value);
	*value = atoi(string_value);
	DPRINTF("Integer form of value is %d.\n", *value);
	return OP_SUCCESS;
}

/**
 * Get environment variables SGSH_IN, SGSH_OUT set up by
 * the shell (through execvpe()).
 */
static int
get_environment_vars()
{
	DPRINTF("Try to get environment variable SGSH_IN.\n");
	if (get_env_var("SGSH_IN", &self_node.sgsh_in) == OP_ERROR) 
		return OP_ERROR;
	DPRINTF("Try to get environment variable SGSH_OUT.\n");
	if (get_env_var("SGSH_OUT", &self_node.sgsh_out) == OP_ERROR) 
		return OP_ERROR;
	return OP_SUCCESS;
}

/**
 * Each tool in the sgsh graph calls sgsh_negotiate() to take part in
 * peer-to-peer negotiation. A message block (MB) is circulated among tools
 * and is filled with tools' I/O requirements. When all requirements are in 
 * place, an algorithm runs that tries to find a solution that satisfies
 * all requirements. If a solution is found, pipes are allocated and
 * set up according to the solution. The appropriate file descriptors
 * are provided to each tool and the negotiation phase ends.
 * The function's return value signifies success or failure of the
 * negotiation phase.
 */
int
sgsh_negotiate(const char *tool_name, /* Input. Try remove. */
                    int channels_required, /* How many input channels can take. */
                    int channels_provided, /* How many output channels can 
						provide. */
                                     /* Output: to fill. */
                    int **input_fds,  /* Input file descriptors. */
                    int *n_input_fds, /* Number of input file descriptors. */
                    int **output_fds, /* Output file descriptors. */
                    int *n_output_fds) /* Number of output file descriptors. */
		    /* magic_no? */
{
	int negotiation_round = 0;
	int should_transmit_mb = 1;
	int updated_mb_serial_no = 1;
	pid_t self_pid = getpid(); /* Get tool's pid */
	int buf_size = getpagesize(); /* Make buffer page-wide. */
	char buf[buf_size];
	struct sgsh_negotiation *fresh_mb = NULL; /* MB just read. */
	memset(buf, 0, buf_size); /* Clear buffer used to read/write messages.*/
	DPRINTF("Tool %s with pid %d entered sgsh negotiation.\n", tool_name,
							(int)self_pid);
	
	if (get_environment_vars() == OP_ERROR) {
		err(1, "Failed to extract SGSH_IN, SGSH_OUT environment variables.");
		return PROT_STATE_ERROR;
	}
	
	/* Start negotiation. */
        if ((self_node.sgsh_out) && (!self_node.sgsh_in)) { 
                if (construct_message_block(self_pid) == OP_ERROR) 
			return PROT_STATE_ERROR;
                self_dispatcher.fd_direction = STDOUT_FILENO;
        } else { /* or wait to receive MB. */
		chosen_mb = NULL;
		if (try_read_message_block(buf, buf_size, fresh_mb) == OP_ERROR)
			return PROT_STATE_ERROR;
		chosen_mb = fresh_mb;
	}
	
	/* Create sgsh node representation and add node, edge to the graph. */
	fill_sgsh_node(tool_name, self_pid, channels_required, 
						channels_provided);
	if (try_add_sgsh_node() == OP_ERROR) return PROT_STATE_ERROR;
	if (try_add_sgsh_edge() == OP_ERROR) return PROT_STATE_ERROR;
	
	/* Perform negotiation rounds. */
	while (chosen_mb->state_flag == PROT_STATE_NEGOTIATION) {
		check_negotiation_round(&negotiation_round, 
					updated_mb_serial_no);
		if (should_transmit_mb) 
			if (write_mb(buf, buf_size) == OP_ERROR) 
				return OP_ERROR;
		if (chosen_mb->state_flag == PROT_STATE_NEGOTIATION_END) 
			break; /* Did spread the word,now leave.*/
		if (try_read_message_block(buf, buf_size, fresh_mb) == OP_ERROR)
			return PROT_STATE_ERROR;
		if (compete_message_block(fresh_mb, &should_transmit_mb,
					&updated_mb_serial_no) == OP_ERROR)
			return PROT_STATE_ERROR;
	}

	/* Solve problem and allocate pipes. */
	if (allocate_io_connections(input_fds, n_input_fds, output_fds, 
						n_output_fds) == OP_ERROR)
		return PROT_STATE_ERROR;

	return chosen_mb->state_flag;
}
