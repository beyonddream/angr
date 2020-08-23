#ifndef SIM_UNICORN_HPP
#define SIM_UNICORN_HPP

extern "C" {
	#include <libvex_guest_offsets.h>
}

#define PAGE_SIZE 0x1000
#define PAGE_SHIFT 12

// Maximum size of a qemu/unicorn basic block
// See State::step for why this is necessary
static const uint32_t MAX_BB_SIZE = 800;

static const uint8_t MAX_MEM_ACCESS_SIZE = 8;

// The size of the longest register in archinfo's uc_regs for all architectures
static const uint8_t MAX_REGISTER_BYTE_SIZE = 32;

typedef enum taint: uint8_t {
	TAINT_NONE = 0,
	TAINT_DIRTY = 1,
	TAINT_SYMBOLIC = 2,
} taint_t;

typedef enum : uint8_t {
	TAINT_ENTITY_REG = 0,
	TAINT_ENTITY_TMP = 1,
	TAINT_ENTITY_MEM = 2,
	TAINT_ENTITY_NONE = 3,
} taint_entity_enum_t;

typedef enum : uint8_t {
	TAINT_STATUS_CONCRETE = 0,
	TAINT_STATUS_DEPENDS_ON_READ_FROM_SYMBOLIC_ADDR,
	TAINT_STATUS_SYMBOLIC,
} taint_status_result_t;

typedef uint64_t address_t;
typedef uint64_t unicorn_reg_id_t;
typedef uint64_t vex_reg_offset_t;
typedef uint64_t vex_tmp_id_t;

typedef struct taint_entity_t {
	taint_entity_enum_t entity_type;

	// The actual entity data. Only one of them is valid at a time depending on entity_type.
	// This could have been in a union but std::vector has a constructor and datatypes with
	// constructors are not allowed inside unions
	// VEX Register ID
	vex_reg_offset_t reg_offset;
	// VEX temp ID
	vex_tmp_id_t tmp_id;
	// List of registers and VEX temps. Used in case of memory references.
	std::vector<taint_entity_t> mem_ref_entity_list;
	// Instruction in which the entity is used. Used for taint sinks; ignored for taint sources.
	address_t instr_addr;

	bool operator==(const taint_entity_t &other_entity) const {
		if (entity_type != other_entity.entity_type) {
			return false;
		}
		if (entity_type == TAINT_ENTITY_REG) {
			return (reg_offset == other_entity.reg_offset);
		}
		if (entity_type == TAINT_ENTITY_TMP) {
			return (tmp_id == other_entity.tmp_id);
		}
		return (mem_ref_entity_list == other_entity.mem_ref_entity_list);
	}

	// Hash function for use in unordered_map. Defined in class and invoked from hash struct.
	// TODO: Check performance of hash and come up with better one if too bad
	std::size_t operator()(const taint_entity_t &taint_entity) const {
		if (taint_entity.entity_type == TAINT_ENTITY_REG) {
			return std::hash<uint64_t>()(taint_entity.entity_type) ^
				   std::hash<uint64_t>()(taint_entity.reg_offset);
		}
		else if (taint_entity.entity_type == TAINT_ENTITY_TMP) {
			return std::hash<uint64_t>()(taint_entity.entity_type) ^
				   std::hash<uint64_t>()(taint_entity.tmp_id);
		}
		else if (taint_entity.entity_type == TAINT_ENTITY_MEM) {
			std::size_t taint_entity_hash = std::hash<uint64_t>()(taint_entity.entity_type);
			for (auto &sub_entity: taint_entity.mem_ref_entity_list) {
				taint_entity_hash ^= sub_entity.operator()(sub_entity);
			}
			return taint_entity_hash;
		}
		else {
			return std::hash<uint64_t>()(taint_entity.entity_type);
		}
	}
} taint_entity_t;

// Hash function for unordered_map. Needs to be defined this way in C++.
template <>
struct std::hash<taint_entity_t> {
	std::size_t operator()(const taint_entity_t &entity) const {
		return entity.operator()(entity);
	}
};

typedef struct {
	address_t address;
	uint8_t value[MAX_MEM_ACCESS_SIZE]; // Assume size of read is not more than 8 just like write
	size_t size;
	bool is_value_symbolic;
} mem_read_result_t;

typedef struct memory_value_t {
	uint64_t address;
    uint8_t value[MAX_MEM_ACCESS_SIZE];
    uint64_t size;

	bool operator==(const memory_value_t &other_mem_value) {
		if ((address != other_mem_value.address) || (size != other_mem_value.size)) {
			return false;
		}
		return (memcmp(value, other_mem_value.value, size) == 0);
	}
} memory_value_t;

typedef struct {
	uint64_t offset;
	uint8_t value[MAX_REGISTER_BYTE_SIZE];
} register_value_t;

typedef struct instr_details_t {
	address_t instr_addr;
	bool has_memory_dep;
	memory_value_t memory_value;

	bool operator==(const instr_details_t &other_instr) {
		return ((instr_addr == other_instr.instr_addr) && (has_memory_dep == other_instr.has_memory_dep) &&
			(memory_value == other_instr.memory_value));
	}

	bool operator<(const instr_details_t &other_instr) const {
		return (instr_addr < other_instr.instr_addr);
	}
} instr_details_t;

typedef struct {
	address_t block_addr;
	uint64_t block_size;
	std::vector<instr_details_t> symbolic_instrs;
	std::vector<register_value_t> register_values;
	bool vex_lift_failed;

	void reset() {
		block_addr = 0;
		block_size = 0;
		vex_lift_failed = false;
		symbolic_instrs.clear();
		register_values.clear();
	}
} block_details_t;

// This struct is used only to return data to the state plugin since ctypes doesn't natively handle
// C++ STL containers
typedef struct {
	uint64_t block_addr;
    uint64_t block_size;
    instr_details_t *symbolic_instrs;
    uint64_t symbolic_instrs_count;
    register_value_t *register_values;
    uint64_t register_values_count;
} block_details_ret_t;

typedef enum stop {
	STOP_NORMAL=0,
	STOP_STOPPOINT,
	STOP_ERROR,
	STOP_SYSCALL,
	STOP_EXECNONE,
	STOP_ZEROPAGE,
	STOP_NOSTART,
	STOP_SEGFAULT,
	STOP_ZERO_DIV,
	STOP_NODECODE,
	STOP_HLT,
	STOP_VEX_LIFT_FAILED,
	STOP_SYMBOLIC_CONDITION,
	STOP_SYMBOLIC_PC,
	STOP_SYMBOLIC_READ_ADDR,
	STOP_SYMBOLIC_READ_SYMBOLIC_TRACKING_DISABLED,
	STOP_SYMBOLIC_WRITE_ADDR,
	STOP_SYMBOLIC_BLOCK_EXIT_STMT,
	STOP_MULTIPLE_MEMORY_READS,
	STOP_UNSUPPORTED_STMT_PUTI,
	STOP_UNSUPPORTED_STMT_STOREG,
	STOP_UNSUPPORTED_STMT_LOADG,
	STOP_UNSUPPORTED_STMT_CAS,
	STOP_UNSUPPORTED_STMT_LLSC,
	STOP_UNSUPPORTED_STMT_DIRTY,
	STOP_UNSUPPORTED_STMT_UNKNOWN,
	STOP_UNSUPPORTED_EXPR_GETI,
	STOP_UNSUPPORTED_EXPR_UNKNOWN,
	STOP_UNKNOWN_MEMORY_WRITE,
	STOP_UNKNOWN_MEMORY_READ,
} stop_t;

typedef std::vector<std::pair<taint_entity_t, std::unordered_set<taint_entity_t>>> taint_vector_t;

typedef struct instruction_taint_entry_t {
	// List of direct taint sources for a taint sink
	taint_vector_t taint_sink_src_map;

	// List of registers a taint sink depends on
	std::unordered_set<taint_entity_t> dependencies_to_save;

	// List of taint entities in ITE expression's condition, if any
	std::unordered_set<taint_entity_t> ite_cond_entity_list;

	// List of registers modified by instruction and whether register's final value depends on
	// it's previous value
	std::vector<std::pair<vex_reg_offset_t, bool>> modified_regs;

	bool has_memory_read;
	bool has_memory_write;

	bool operator==(const instruction_taint_entry_t &other_instr_deps) const {
		return (taint_sink_src_map == other_instr_deps.taint_sink_src_map) &&
			   (dependencies_to_save == other_instr_deps.dependencies_to_save) &&
			   (has_memory_read == other_instr_deps.has_memory_read) &&
			   (has_memory_write == other_instr_deps.has_memory_write);
	}

	void reset() {
		dependencies_to_save.clear();
		ite_cond_entity_list.clear();
		taint_sink_src_map.clear();
		modified_regs.clear();
		has_memory_read = false;
		has_memory_write = false;
		return;
	}
} instruction_taint_entry_t;

typedef struct block_taint_entry_t {
	std::map<address_t, instruction_taint_entry_t> block_instrs_taint_data_map;
	std::unordered_set<taint_entity_t> exit_stmt_guard_expr_deps;
	address_t exit_stmt_instr_addr;
	bool has_unsupported_stmt_or_expr_type;
	stop_t unsupported_stmt_stop_reason;

	bool operator==(const block_taint_entry_t &other_entry) const {
		return (block_instrs_taint_data_map == other_entry.block_instrs_taint_data_map) &&
			   (exit_stmt_guard_expr_deps == other_entry.exit_stmt_guard_expr_deps);
	}
} block_taint_entry_t;

typedef struct {
	stop_t stop_reason;
	address_t block_addr;
	uint64_t block_size;
} stop_details_t;

typedef struct {
	std::unordered_set<taint_entity_t> sources;
	std::unordered_set<taint_entity_t> ite_cond_entities;
	bool has_unsupported_expr;
	stop_t unsupported_expr_stop_reason;
} taint_sources_and_and_ite_cond_t;

typedef struct {
	std::set<instr_details_t> dependent_instrs;
	std::unordered_set<vex_reg_offset_t> concrete_registers;
} instr_slice_details_t;

typedef struct CachedPage {
	size_t size;
	uint8_t *bytes;
	uint64_t perms;
} CachedPage;

typedef taint_t PageBitmap[PAGE_SIZE];
typedef std::map<address_t, CachedPage> PageCache;
typedef std::unordered_map<address_t, block_taint_entry_t> BlockTaintCache;
typedef struct caches {
	PageCache *page_cache;
} caches_t;
std::map<uint64_t, caches_t> global_cache;

typedef std::unordered_set<vex_reg_offset_t> RegisterSet;
typedef std::unordered_map<vex_reg_offset_t, unicorn_reg_id_t> RegisterMap;
typedef std::unordered_set<vex_tmp_id_t> TempSet;

typedef struct mem_access {
	address_t address;
	uint8_t value[MAX_MEM_ACCESS_SIZE]; // assume size of any memory write is no more than 8
	int size;
	int clean; // save current page bitmap
	bool is_symbolic;
} mem_access_t; // actually it should be `mem_write_t` :)

typedef struct mem_update {
	address_t address;
	uint64_t length;
	struct mem_update *next;
} mem_update_t;

typedef struct transmit_record {
	void *data;
	uint32_t count;
} transmit_record_t;

// These prototypes may be found in <unicorn/unicorn.h> by searching for "Callback"
static void hook_mem_read(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *user_data);
static void hook_mem_write(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *user_data);
static bool hook_mem_unmapped(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *user_data);
static bool hook_mem_prot(uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value, void *user_data);
static void hook_block(uc_engine *uc, uint64_t address, int32_t size, void *user_data);
static void hook_intr(uc_engine *uc, uint32_t intno, void *user_data);

class State {
	uc_engine *uc;
	PageCache *page_cache;
	BlockTaintCache block_taint_cache;
	bool hooked;

	uc_context *saved_regs;

	std::vector<mem_access_t> mem_writes;
	// List of all memory writes and their taint status
	// Memory write instruction address -> is_symbolic
	// TODO: Need to modify memory write taint handling for architectures that perform multiple
	// memory writes in a single instruction
	std::unordered_map<address_t, bool> mem_writes_taint_map;

	// Slice of current block to set the value of a register
	std::unordered_map<vex_reg_offset_t, std::vector<instr_details_t>> reg_instr_slice;

	// Slice of current block for an instruction
	std::unordered_map<address_t, instr_slice_details_t> instr_slice_details_map;

	// List of instructions in a block that should be executed symbolically. These are stored
	// separately for easy rollback in case of errors.
	block_details_t block_details;

	// List of registers which are concrete dependencies of a block's instructions executed symbolically
	std::unordered_set<vex_reg_offset_t> block_concrete_dependencies;

	// List of register values at start of block
	std::unordered_map<vex_reg_offset_t, register_value_t> block_start_reg_values;

	// Similar to memory reads in a block, we track the state of registers and VEX temps when
	// propagating taint in a block for easy rollback if we need to abort due to read from/write to
	// a symbolic address
	RegisterSet block_symbolic_registers, block_concrete_registers;
	TempSet block_symbolic_temps;

	std::map<address_t, taint_t *> active_pages;
	std::set<address_t> stop_points;

	address_t taint_engine_next_instr_address, taint_engine_mem_read_stop_instruction;

	address_t unicorn_next_instr_addr;

public:
	std::vector<address_t> bbl_addrs;
	std::vector<address_t> stack_pointers;
	std::unordered_set<address_t> executed_pages;
	std::unordered_set<address_t>::iterator *executed_pages_iterator;
	uint64_t syscall_count;
	std::vector<transmit_record_t> transmit_records;
	uint64_t cur_steps, max_steps;
	uc_hook h_read, h_write, h_block, h_prot, h_unmap, h_intr;
	bool stopped;

	bool ignore_next_block;
	bool ignore_next_selfmod;
	address_t cur_address;
	int32_t cur_size;

	uc_arch arch;
	uc_mode mode;
	bool interrupt_handled;
	uint32_t transmit_sysno;
	uint32_t transmit_bbl_addr;

	VexArch vex_guest;
	VexArchInfo vex_archinfo;
	RegisterSet symbolic_registers; // tracking of symbolic registers
	RegisterSet blacklisted_registers;  // Registers which shouldn't be saved as a concrete dependency
	RegisterMap vex_to_unicorn_map; // Mapping of VEX offsets to unicorn registers
	RegisterMap vex_sub_reg_map; // Mapping of VEX sub-registers to their main register
	std::unordered_map<vex_reg_offset_t, uint64_t> reg_size_map;
	RegisterSet artificial_vex_registers; // Artificial VEX registers
	std::unordered_map<vex_reg_offset_t, uint64_t> cpu_flags;	// VEX register offset and bitmask for CPU flags
	int64_t cpu_flags_register;
	stop_details_t stop_details;

	// Result of all memory reads executed. Instruction address -> memory read result
	std::unordered_map<address_t, mem_read_result_t> mem_reads_map;

	// List of instructions that should be executed symbolically
	std::vector<block_details_t> blocks_with_symbolic_instrs;

	bool track_bbls;
	bool track_stack;

	State(uc_engine *_uc, uint64_t cache_key);

	~State() {
		for (auto it = active_pages.begin(); it != active_pages.end(); it++) {
			// only poor guys consider about memory leak :(
			//LOG_D("delete active page %#lx", it->first);
			// delete should use the bracket operator since PageBitmap is an array typedef
			delete[] it->second;
		}
		active_pages.clear();
		uc_free(saved_regs);
	}

	void hook();

	void unhook();

	uc_err start(address_t pc, uint64_t step = 1);

	void stop(stop_t reason);

	void step(address_t current_address, int32_t size, bool check_stop_points=true);

	/*
	 * record current memory write
	 */
	bool log_write(address_t address, int size, int clean, bool is_write_symbolic);

	/*
	 * commit all memory actions.
	 * end_block denotes whether this is done at the end of the block or not
	 */
	void commit();

	/*
	 * undo recent memory actions.
	 */
	void rollback();

	taint_t *page_lookup(address_t address) const;

	/*
	 * allocate a new PageBitmap and put into active_pages.
	 */
	void page_activate(address_t address, uint8_t *taint = NULL, uint64_t taint_offset = 0);

	/*
	 * record consecutive dirty bit rage, return a linked list of ranges
	 */
	mem_update_t *sync();

	/*
	 * set a list of stops to stop execution at
	 */

	void set_stops(uint64_t count, address_t *stops);

	std::pair<address_t, size_t> cache_page(address_t address, size_t size, char* bytes, uint64_t permissions);

    void wipe_page_from_cache(address_t address);

    void uncache_pages_touching_region(address_t address, uint64_t length);

    void clear_page_cache();

	bool map_cache(address_t address, size_t size);

	bool in_cache(address_t address) const;

	/*
	 * Finds tainted data in the provided range and returns the address.
	 * Returns -1 if no tainted data is present.
	 */
	int64_t find_tainted(address_t address, int size);

	void handle_write(address_t address, int size, bool is_interrupt);

	std::pair<std::unordered_set<taint_entity_t>, bool> compute_dependencies_to_save(const std::unordered_set<taint_entity_t> &taint_sources) const;

	void compute_slice_of_instrs(address_t instr_addr, const instruction_taint_entry_t &instr_taint_entry);

	block_taint_entry_t process_vex_block(IRSB *vex_block, address_t address);

	void get_register_value(uint64_t vex_reg_offset, uint8_t *out_reg_value) const;

	/*
	 * Given an IR expression, computes the taint sources and list of taint entities in ITE condition expression, if any
	 */
	taint_sources_and_and_ite_cond_t get_taint_sources_and_ite_cond(IRExpr *expr, address_t instr_addr, bool is_exit_stmt);

	/*
	 * Determine cumulative result of taint statuses of a set of taint entities
	 */
	taint_status_result_t get_final_taint_status(const std::unordered_set<taint_entity_t> &taint_sources) const;

	/*
	 * A vector version of get_final_taint_status for checking mem_ref_entity_list which
	 * can't be an unordered_set
	 */
	taint_status_result_t get_final_taint_status(const std::vector<taint_entity_t> &taint_sources) const;

	void mark_register_symbolic(vex_reg_offset_t reg_offset, bool do_block_level);

	void mark_temp_symbolic(vex_tmp_id_t temp_id);

	void mark_register_concrete(vex_reg_offset_t reg_offset, bool do_block_level);

	bool is_symbolic_register(vex_reg_offset_t reg_offset) const;

	bool is_symbolic_temp(vex_tmp_id_t temp_id) const;

	bool is_symbolic_register_or_temp(const taint_entity_t &entity) const;

	void propagate_taints();

	void propagate_taint_of_mem_read_instr(const address_t instr_addr);
	
	void propagate_taint_of_one_instr(address_t instr_addr, const instruction_taint_entry_t &instr_taint_entry);

	instr_details_t compute_instr_details(address_t instr_addr, const instruction_taint_entry_t &instr_taint_entry) const;

	void read_memory_value(address_t address, uint64_t size, uint8_t *result, size_t result_size) const;

	void start_propagating_taint(address_t block_address, int32_t block_size);

	void continue_propagating_taint();

	void update_register_slice(address_t instr_addr, const instruction_taint_entry_t &curr_instr_taint_entry);

	bool is_block_exit_guard_symbolic() const;

	address_t get_instruction_pointer();

	address_t get_stack_pointer();

	// Inline functions

	/*
	 * Feasibility checks for unicorn
	 */

	inline bool is_symbolic_tracking_disabled() {
		return (vex_guest == VexArch_INVALID);
	}

	inline bool is_symbolic_taint_propagation_disabled() {
		return (is_symbolic_tracking_disabled() || block_details.vex_lift_failed);
	}

	inline vex_reg_offset_t get_full_register_offset(vex_reg_offset_t reg_offset) {
		auto vex_sub_reg_mapping_entry = vex_sub_reg_map.find(reg_offset);
		if (vex_sub_reg_mapping_entry != vex_sub_reg_map.end()) {
			return vex_sub_reg_mapping_entry->second;
		}
		return reg_offset;
	}

	inline address_t get_taint_engine_mem_read_stop_instruction() const {
		return taint_engine_mem_read_stop_instruction;
	}

	inline bool is_valid_dependency_register(vex_reg_offset_t reg_offset) const {
		return ((artificial_vex_registers.count(reg_offset) == 0) && (blacklisted_registers.count(reg_offset) == 0));
	}

	inline bool is_blacklisted_register(vex_reg_offset_t reg_offset) const {
		return (blacklisted_registers.count(reg_offset) > 0);
	}

	inline unsigned int arch_pc_reg_vex_offset() {
		switch (arch) {
			case UC_ARCH_X86:
				return mode == UC_MODE_64 ? OFFSET_amd64_RIP : OFFSET_x86_EIP;
			case UC_ARCH_ARM:
				return OFFSET_arm_R15T;
			case UC_ARCH_ARM64:
				return OFFSET_arm64_PC;
			case UC_ARCH_MIPS:
				return mode == UC_MODE_64 ? OFFSET_mips64_PC : OFFSET_mips32_PC;
			default:
				return -1;
		}
	}

	inline unsigned int arch_pc_reg() {
		switch (arch) {
			case UC_ARCH_X86:
				return mode == UC_MODE_64 ? UC_X86_REG_RIP : UC_X86_REG_EIP;
			case UC_ARCH_ARM:
				return UC_ARM_REG_PC;
			case UC_ARCH_ARM64:
				return UC_ARM64_REG_PC;
			case UC_ARCH_MIPS:
				return UC_MIPS_REG_PC;
			default:
				return -1;
		}
	}

	inline unsigned int arch_sp_reg() {
		switch (arch) {
			case UC_ARCH_X86:
				return mode == UC_MODE_64 ? UC_X86_REG_RSP : UC_X86_REG_ESP;
			case UC_ARCH_ARM:
				return UC_ARM_REG_SP;
			case UC_ARCH_ARM64:
				return UC_ARM64_REG_SP;
			case UC_ARCH_MIPS:
				return UC_MIPS_REG_SP;
			default:
				return -1;
		}
	}
};


#define CTYPES_INTERFACE_HPP

/*
 * C style bindings makes it simple and dirty
 */

extern "C"
State *simunicorn_alloc(uc_engine *uc, uint64_t cache_key);

extern "C"
void simunicorn_dealloc(State *state);

extern "C"
uint64_t *simunicorn_bbl_addrs(State *state);

extern "C"
uint64_t *simunicorn_stack_pointers(State *state);

extern "C"
uint64_t simunicorn_bbl_addr_count(State *state);

extern "C"
uint64_t simunicorn_syscall_count(State *state);

extern "C"
void simunicorn_hook(State *state);

extern "C"
void simunicorn_unhook(State *state);

extern "C"
uc_err simunicorn_start(State *state, uint64_t pc, uint64_t step);

extern "C"
void simunicorn_stop(State *state, stop_t reason);

extern "C"
mem_update_t *simunicorn_sync(State *state);

extern "C"
void simunicorn_destroy(mem_update_t * head);

extern "C"
uint64_t simunicorn_step(State *state);

extern "C"
void simunicorn_set_stops(State *state, uint64_t count, uint64_t *stops);

extern "C"
void simunicorn_activate(State *state, uint64_t address, uint64_t length, uint8_t *taint);

extern "C"
uint64_t simunicorn_executed_pages(State *state);

//
// Stop analysis
//

extern "C"
stop_details_t simunicorn_get_stop_details(State *state);

//
// Symbolic register tracking
//

extern "C"
void simunicorn_symbolic_register_data(State *state, uint64_t count, uint64_t *offsets);

extern "C"
uint64_t simunicorn_get_symbolic_registers(State *state, uint64_t *output);

extern "C"
void simunicorn_enable_symbolic_reg_tracking(State *state, VexArch guest, VexArchInfo archinfo);

extern "C"
void simunicorn_disable_symbolic_reg_tracking(State *state);

//
// Concrete transmits
//

extern "C"
bool simunicorn_is_interrupt_handled(State *state);

extern "C"
void simunicorn_set_transmit_sysno(State *state, uint32_t sysno, uint64_t bbl_addr);

extern "C"
transmit_record_t *simunicorn_process_transmit(State *state, uint32_t num);

/*
 * Page cache
 */

extern "C"
bool simunicorn_cache_page(State *state, uint64_t address, uint64_t length, char *bytes, uint64_t permissions);

extern "C"
void simunicorn_uncache_pages_touching_region(State *state, uint64_t address, uint64_t length);

extern "C"
void simunicorn_clear_page_cache(State *state);

// Tracking settings
extern "C"
void simunicorn_set_tracking(State *state, bool track_bbls, bool track_stack);

extern "C"
bool simunicorn_in_cache(State *state, uint64_t address);

// VEX artificial registers list
extern "C"
void simunicorn_set_artificial_registers(State *state, uint64_t *offsets, uint64_t count);

// Register sizes mapping
extern "C"
void simunicorn_set_vex_offset_to_register_size_mapping(State *state, uint64_t *vex_offsets, uint64_t *reg_sizes, uint64_t count);

// VEX register offsets to unicorn register ID mappings
extern "C"
void simunicorn_set_vex_to_unicorn_reg_mappings(State *state, uint64_t *vex_offsets, uint64_t *unicorn_ids, uint64_t count);

// VEX sub-registers to full register mapping
extern "C"
void simunicorn_set_vex_sub_reg_to_reg_mappings(State *state, uint64_t *vex_sub_reg_offsets, uint64_t *vex_reg_offsets, uint64_t count);

// Mapping details for flags registers
extern "C"
void simunicorn_set_cpu_flags_details(State *state, uint64_t *flag_vex_id, uint64_t *bitmasks, uint64_t count);

// Flag register ID in unicorn
extern "C"
void simunicorn_set_unicorn_flags_register_id(State *state, int64_t reg_id);

extern "C"
void simunicorn_set_register_blacklist(State *state, uint64_t *reg_list, uint64_t count);

// VEX re-execution data
extern "C"
uint64_t simunicorn_get_count_of_blocks_with_symbolic_instrs(State *state);

extern "C"
void simunicorn_get_details_of_blocks_with_symbolic_instrs(State *state, block_details_ret_t *ret_block_details);

#endif /* SIM_UNICORN_HPP */
