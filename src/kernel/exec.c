#include <elf.h>
#include <common/string.h>
#include <common/defines.h>
#include <kernel/console.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/pt.h>
#include <kernel/mem.h>
#include <kernel/paging.h>
#include <aarch64/trap.h>
#include <fs/file.h>
#include <fs/inode.h>
#include <kernel/printk.h>

//static u64 auxv[][2] = {{AT_PAGESZ, PAGE_SIZE}};
extern int fdalloc(struct file* f);
void trap_return();

int execve(const char *path, char *const argv[], char *const envp[]) {
	// TODO
	OpContext ctx;
	bcache.begin_op(&ctx);
	Inode* node = namei(path, &ctx);

	if(!node){
		bcache.end_op(&ctx);
		printk("(Error): %s not found\n", path);
		return -1;
	}

	unsigned char e_ident[EI_NIDENT];
	inodes.lock(node);
	inodes.read(node, (u8*)e_ident, 0, sizeof(unsigned char) * EI_NIDENT);

	// check the e_ident and ignore the 32-bit format
	if (strncmp((const char*)e_ident, ELFMAG, 4) != 0 ||
		e_ident[EI_CLASS] != 2){
		inodes.unlock(node);
		inodes.put(&ctx, node);
		bcache.end_op(&ctx);
		printk("(Error):File format not supported");
		return -1;
	}
	else{
		// only 64-bit format implemented

		// read elf header
		Elf64_Ehdr* ehdr = (Elf64_Ehdr*)kalloc(sizeof(Elf64_Ehdr));
		if(inodes.read(node, (u8*)ehdr, 0, sizeof(Elf64_Ehdr)) != sizeof(Elf64_Ehdr)){
			inodes.unlock(node);
			inodes.put(&ctx, node);
			bcache.end_op(&ctx);
			printk("(Error): Elf header maybe corrupted");
			return -1;
		};
		
		// simple check of the elf file
		if(sizeof(Elf64_Phdr) != ehdr->e_phentsize){
			kfree(ehdr);
			inodes.unlock(node);
			inodes.put(&ctx, node);
			bcache.end_op(&ctx);
			printk("(Error): Elf file is corrupted");
			return -1;
		}

		// read all program headers
		// map them to corresponding section of new_pd
		struct pgdir* new_pd = (struct pgdir*)kalloc(sizeof(struct pgdir));
		init_pgdir(new_pd);

		Elf64_Phdr phdr;
		u64 top_of_sections = 0;

		for(auto i = 0; i < ehdr->e_phnum; i++){
			if(inodes.read(node, (u8*)&phdr, ehdr->e_phoff + i * sizeof(Elf64_Phdr), sizeof(Elf64_Phdr)) != sizeof(Elf64_Phdr)){
				inodes.unlock(node);
				inodes.put(&ctx, node);
				bcache.end_op(&ctx);
				free_pgdir(new_pd);
				printk("(Error): Failed to read program header");
				return -1;
			}
			// ignore unloadable program headers
			if(phdr.p_type == PT_LOAD){
				// record the top of sections
				top_of_sections = MAX(top_of_sections, phdr.p_vaddr + phdr.p_memsz);

				// init corresponding section
				struct section* st = (struct section*)kalloc(sizeof(struct section));
				memset(st, 0, sizeof(struct section));
				init_list_node(&st->stnode);
				_insert_into_list(&new_pd->section_head, &st->stnode);
				st->begin = phdr.p_paddr;
				switch (phdr.p_flags){
					case PF_R | PF_X:
						// text section
						st->flags = ST_TEXT;
						st->end = st->begin + phdr.p_filesz;
						break;
					case PF_R | PF_W:
						// data and bss sections
						st->flags = ST_DATA;
						st->end = st->begin + phdr.p_memsz;
						break;
					default:
						inodes.unlock(node);
						inodes.put(&ctx, node);
						bcache.end_op(&ctx);
						free_pgdir(new_pd);
						printk("(Error): Invalid program header type");
						return -1;
				}
				if(st->flags == ST_TEXT){
					// Lazy Allocation
					// set the file and offset
					st->fp = file_alloc();
					st->fp->ip = inodes.share(node);
					st->fp->readable = true;
					st->fp->writable = false;
					st->fp->ref = 1;
					st->fp->off = 0;
					st->fp->type = FD_INODE;
					st->length = phdr.p_filesz;
					st->offset = phdr.p_offset;
				}
				else{
					// map data and bss section
					// [p_vaddr + p_filesz, p_vaddr + p_memsz) is the bss section
					// set bss section to 0
					u64 fsz = phdr.p_filesz, offset = phdr.p_offset, va = phdr.p_vaddr;
					while(fsz){
						u64 cur_page_left = PAGE_SIZE - VA_OFFSET(va);
						u64 cur_writable_sz = MIN(fsz, cur_page_left);
						void *p = kalloc_page();
						memset(p, 0, PAGE_SIZE);
						vmmap(new_pd, PAGE_BASE(va), p, PTE_USER_DATA | PTE_RW);
						if(inodes.read(node, (u8*)(p + VA_OFFSET(va)), offset, cur_writable_sz) != cur_writable_sz){
							inodes.unlock(node);
							inodes.put(&ctx, node);
							bcache.end_op(&ctx);
							free_pgdir(new_pd);
							printk("(Error): Failed to read data and bss section\n");
							return -1;
						}
						fsz -= cur_writable_sz;
						offset += cur_writable_sz;
						va += cur_writable_sz;
					}

					// va now is p_vaddr + p_filesz
					ASSERT(va == phdr.p_vaddr + phdr.p_filesz);
					if(va != PAGE_BASE(va)){
						// current page is already set to 0
						// change to next possible page
						va = PAGE_BASE(va) + PAGE_SIZE;
					}
					
					if(phdr.p_memsz > va - phdr.p_paddr){
						fsz = phdr.p_memsz - (va - phdr.p_vaddr);
						while(fsz > 0){
							// there are still pages of bss section
							u64 cur_write_sz = MIN(PAGE_SIZE - VA_OFFSET(va), fsz);
							vmmap(new_pd, PAGE_BASE(va), get_zero_page(), PTE_USER_DATA | PTE_RO);
							fsz -= cur_write_sz;
							va += cur_write_sz;
						}
						// va now is p_vaddr + p_memsz
						ASSERT(va == phdr.p_vaddr + phdr.p_memsz);
					}
				}
			}
		}
		inodes.unlock(node);
		inodes.put(&ctx, node);
		bcache.end_op(&ctx);

		// init the heap section
		_for_in_list(p, &new_pd->section_head){
			if(p == &new_pd->section_head)continue;
			struct section* st = container_of(p, struct section, stnode);
			if(st->flags == ST_HEAP){
				st->begin = st->end = PAGE_BASE(top_of_sections) + PAGE_SIZE;
				// make sure there are enough space for user stack
				ASSERT(st->end < TOP_USER_STACK - USER_STACK_SIZE);
				break;
			}
		} 

		// create and init user stack
		u64 sp = TOP_USER_STACK - RESERVED_SIZE;	// reserved
		for(u64 i = 1; i <= USER_STACK_SIZE / PAGE_SIZE; i++){
			void* p = kalloc_page();
			memset(p, 0, PAGE_SIZE);
			vmmap(new_pd, TOP_USER_STACK - i * PAGE_SIZE, p, PTE_USER_DATA | PTE_RW);
		}

		struct section* st_ustack = (struct section*)kalloc(sizeof(struct section));
		memset(st_ustack, 0, sizeof(struct section));
		st_ustack->begin = TOP_USER_STACK - USER_STACK_SIZE;
		st_ustack->end = TOP_USER_STACK;
		st_ustack->flags = ST_USER_STACK;
		init_list_node(&st_ustack->stnode);
		_insert_into_list(&new_pd->section_head, &st_ustack->stnode);

		// fill initial user stack content
		u64 argc = 0, arg_len = 0, envc = 0, env_len = 0, zero = 0;
		if(envp){
			while (envp[envc]){
				env_len += strlen(envp[envc]) + 1;
				envc++;
			}
		}
		if(argv){
			while (argv[argc]){
				arg_len += strlen(argv[argc]) + 1;
				argc++;
			}
		}

		u64 str_tot = env_len + arg_len;
		u64 content_sp_start = TOP_USER_STACK - RESERVED_SIZE - str_tot;
		u64 ptr_tot = (2 + argc + envc + 1) * 8;
		u64 argc_start = (content_sp_start - ptr_tot) & (~0xf);
		if(argc_start < TOP_USER_STACK - USER_STACK_SIZE){
			printk("Too many varibles");
			PANIC();
		}

		u64 argv_start = argc_start + 8;
		sp = argv_start;
		// copy contents to top of user stack
		for(u64 i = 0; i < argc; i++){
			usize len = strlen(argv[i]) + 1;
			copyout(new_pd, (void*)content_sp_start, argv[i], len);
			copyout(new_pd, (void*)argv_start, &content_sp_start, 8);
			content_sp_start += len;
			argv_start += 8;
		}
		copyout(new_pd, (void*)argv_start, &zero, 8);
		argv_start += 8;
		for(u64 i = 0; i < envc; i++){
			usize len = strlen(envp[i]) + 1;
			copyout(new_pd, (void*)content_sp_start, envp[i], len);
			copyout(new_pd, (void*)argv_start, &content_sp_start, 8);
			content_sp_start += len;
			argv_start += 8;
		}
		copyout(new_pd, (void*)argv_start, &zero, 8);

		sp -= 8;
		copyout(new_pd, (void*)sp, &argc, 8);

		auto this = thisproc();
		this->ucontext->sp = sp;

		free_pgdir(&this->pgdir);
		this->ucontext->elr = ehdr->e_entry;
		memcpy(&this->pgdir, new_pd, sizeof(struct pgdir));
		init_list_node(&this->pgdir.section_head);
		_insert_into_list(&new_pd->section_head, &this->pgdir.section_head);
		_detach_from_list(&new_pd->section_head);
		kfree(new_pd);
		attach_pgdir(&this->pgdir);
		return 0;
	}
	PANIC();
}
