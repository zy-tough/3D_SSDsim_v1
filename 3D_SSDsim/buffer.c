/*****************************************************************************************************************************
This is a project on 3D_SSDsim, based on ssdsim under the framework of the completion of structures, the main function:
1.Support for 3D commands, for example:mutli plane\interleave\copyback\program suspend/Resume..etc
2.Multi - level parallel simulation
3.Clear hierarchical interface
4.4-layer structure

FileName： ssd.c
Author: Zuo Lu 		Version: 1.0	Date:2017/04/06
Description: 
buff layer: only contains data cache (minimum processing size for the sector, that is, unit = 512B), mapping table (page-level);

History:
<contributor>     <time>        <version>       <desc>                   <e-mail>
Zuo Lu	        2017/04/06	      1.0		    Creat 3D_SSDsim       617376665@qq.com

*****************************************************************************************************************************/



#define _CRTDBG_MAP_ALLOC

#include <stdlib.h>
#include <crtdbg.h>
#include <stdio.h>
#include <time.h>
#include <string.h>

#include "ssd.h"
#include "initialize.h"
#include "flash.h"
#include "buffer.h"
#include "interface.h"
#include "ftl.h"
#include "fcl.h"


/**********************************************************************************************************************************************
*首先buffer是个写buffer，就是为写请求服务的，因为读flash的时间tR为20us，写flash的时间tprog为200us，所以为写服务更能节省时间
*  读操作：如果命中了buffer，从buffer读，不占用channel的I/O总线，没有命中buffer，从flash读，占用channel的I/O总线，但是不进buffer了
*  写操作：首先request分成sub_request子请求，如果是动态分配，sub_request挂到ssd->sub_request上，因为不知道要先挂到哪个channel的sub_request上
*          如果是静态分配则sub_request挂到channel的sub_request链上,同时不管动态分配还是静态分配sub_request都要挂到request的sub_request链上
*		   因为每处理完一个request，都要在traceoutput文件中输出关于这个request的信息。处理完一个sub_request,就将其从channel的sub_request链
*		   或ssd的sub_request链上摘除，但是在traceoutput文件输出一条后再清空request的sub_request链。
*		   sub_request命中buffer则在buffer里面写就行了，并且将该sub_page提到buffer链头(LRU)，若没有命中且buffer满，则先将buffer链尾的sub_request
*		   写入flash(这会产生一个sub_request写请求，挂到这个请求request的sub_request链上，同时视动态分配还是静态分配挂到channel或ssd的
*		   sub_request链上),在将要写的sub_page写入buffer链头
***********************************************************************************************************************************************/
struct ssd_info *buffer_management(struct ssd_info *ssd)
{
	unsigned int j, lsn, lpn, last_lpn, first_lpn, index, complete_flag = 0, state, full_page;
	unsigned int flag = 0, need_distb_flag, lsn_flag, flag1 = 1, active_region_flag = 0;
	struct request *new_request;
	struct buffer_group *buffer_node, key;
	unsigned int mask = 0, offset1 = 0, offset2 = 0;

#ifdef DEBUG
	printf("enter buffer_management,  current time:%I64u\n", ssd->current_time);
#endif
	ssd->dram->current_time = ssd->current_time;
	full_page = ~(0xffffffff << ssd->parameter->subpage_page);

	new_request = ssd->request_tail;
	lsn = new_request->lsn;
	lpn = new_request->lsn / ssd->parameter->subpage_page;
	last_lpn = (new_request->lsn + new_request->size - 1) / ssd->parameter->subpage_page;
	first_lpn = new_request->lsn / ssd->parameter->subpage_page;

	new_request->need_distr_flag = (unsigned int*)malloc(sizeof(unsigned int)*((last_lpn - first_lpn + 1)*ssd->parameter->subpage_page / 32 + 1));
	alloc_assert(new_request->need_distr_flag, "new_request->need_distr_flag");
	memset(new_request->need_distr_flag, 0, sizeof(unsigned int)*((last_lpn - first_lpn + 1)*ssd->parameter->subpage_page / 32 + 1));

	if (new_request->operation == READ)
	{
		while (lpn <= last_lpn)
		{
			/************************************************************************************************
			*need_distb_flag表示是否需要执行distribution函数，1表示需要执行，buffer中没有，0表示不需要执行
			*即1表示需要分发，0表示不需要分发，对应点初始全部赋为1
			*************************************************************************************************/
			need_distb_flag = full_page;
			key.group = lpn;
			buffer_node = (struct buffer_group*)avlTreeFind(ssd->dram->buffer, (TREE_NODE *)&key);		// buffer node 

			while ((buffer_node != NULL) && (lsn<(lpn + 1)*ssd->parameter->subpage_page) && (lsn <= (new_request->lsn + new_request->size - 1)))
			{
				lsn_flag = full_page;
				mask = 1 << (lsn%ssd->parameter->subpage_page);
				if (mask>31)
				{
					printf("the subpage number is larger than 32!add some cases");
					getchar();
				}
				else if ((buffer_node->stored & mask) == mask)
				{
					flag = 1;
					lsn_flag = lsn_flag&(~mask);
				}

				if (flag == 1)
				{	//如果该buffer节点不在buffer的队首，需要将这个节点提到队首，实现了LRU算法，这个是一个双向队列。		       		
					if (ssd->dram->buffer->buffer_head != buffer_node)
					{
						if (ssd->dram->buffer->buffer_tail == buffer_node)
						{
							buffer_node->LRU_link_pre->LRU_link_next = NULL;
							ssd->dram->buffer->buffer_tail = buffer_node->LRU_link_pre;
						}
						else
						{
							buffer_node->LRU_link_pre->LRU_link_next = buffer_node->LRU_link_next;
							buffer_node->LRU_link_next->LRU_link_pre = buffer_node->LRU_link_pre;
						}
						buffer_node->LRU_link_next = ssd->dram->buffer->buffer_head;
						ssd->dram->buffer->buffer_head->LRU_link_pre = buffer_node;
						buffer_node->LRU_link_pre = NULL;
						ssd->dram->buffer->buffer_head = buffer_node;
					}
					ssd->dram->buffer->read_hit++;
					new_request->complete_lsn_count++;
				}
				else if (flag == 0)
				{
					ssd->dram->buffer->read_miss_hit++;
				}

				need_distb_flag = need_distb_flag&lsn_flag;

				flag = 0;
				lsn++;
			}

			index = (lpn - first_lpn) / (32 / ssd->parameter->subpage_page);
			new_request->need_distr_flag[index] = new_request->need_distr_flag[index] | (need_distb_flag << (((lpn - first_lpn) % (32 / ssd->parameter->subpage_page))*ssd->parameter->subpage_page));
			lpn++;

		}
	}
	else if (new_request->operation == WRITE)
	{
		while (lpn <= last_lpn)
		{
			need_distb_flag = full_page;
			mask = ~(0xffffffff << (ssd->parameter->subpage_page));
			state = mask;

			if (lpn == first_lpn)
			{
				offset1 = ssd->parameter->subpage_page - ((lpn + 1)*ssd->parameter->subpage_page - new_request->lsn);
				state = state&(0xffffffff << offset1);
			}
			if (lpn == last_lpn)
			{
				offset2 = ssd->parameter->subpage_page - ((lpn + 1)*ssd->parameter->subpage_page - (new_request->lsn + new_request->size));
				state = state&(~(0xffffffff << offset2));
			}

			ssd = insert2buffer(ssd, lpn, state, NULL, new_request);
			lpn++;
		}
	}
	complete_flag = 1;
	for (j = 0; j <= (last_lpn - first_lpn + 1)*ssd->parameter->subpage_page / 32; j++)
	{
		if (new_request->need_distr_flag[j] != 0)
		{
			complete_flag = 0;
		}
	}

	/*************************************************************
	*如果请求已经被全部由buffer服务，该请求可以被直接响应，输出结果
	*这里假设dram的服务时间为1000ns
	**************************************************************/
	if ((complete_flag == 1) && (new_request->subs == NULL))
	{
		new_request->begin_time = ssd->current_time;
		new_request->response_time = ssd->current_time + 1000;
	}

	return ssd;
}


/*******************************************************************************
*insert2buffer这个函数是专门为写请求分配子请求服务的在buffer_management中被调用。
********************************************************************************/
struct ssd_info * insert2buffer(struct ssd_info *ssd, unsigned int lpn, int state, struct sub_request *sub, struct request *req)
{
	int write_back_count, flag = 0;                                                             /*flag表示为写入新数据腾空间是否完成，0表示需要进一步腾，1表示已经腾空*/
	unsigned int i, lsn, hit_flag, add_flag, sector_count, active_region_flag = 0, free_sector = 0;
	struct buffer_group *buffer_node = NULL, *pt, *new_node = NULL, key;
	struct sub_request *sub_req = NULL, *update = NULL;


	unsigned int sub_req_state = 0, sub_req_size = 0, sub_req_lpn = 0;

#ifdef DEBUG
	printf("enter insert2buffer,  current time:%I64u, lpn:%d, state:%d,\n", ssd->current_time, lpn, state);
#endif

	sector_count = size(state);                                                                /*需要写到buffer的sector个数*/
	key.group = lpn;
	buffer_node = (struct buffer_group*)avlTreeFind(ssd->dram->buffer, (TREE_NODE *)&key);    /*在平衡二叉树中寻找buffer node*/

	/************************************************************************************************
	*没有命中。
	*第一步根据这个lpn有多少子页需要写到buffer，去除已写回的lsn，为该lpn腾出位置，
	*首先即要计算出free sector（表示还有多少可以直接写的buffer节点）。
	*如果free_sector>=sector_count，即有多余的空间够lpn子请求写，不需要产生写回请求
	*否则，没有多余的空间供lpn子请求写，这时需要释放一部分空间，产生写回请求。就要creat_sub_request()
	*************************************************************************************************/
	if (buffer_node == NULL)
	{
		free_sector = ssd->dram->buffer->max_buffer_sector - ssd->dram->buffer->buffer_sector_count;
		if (free_sector >= sector_count)
		{
			flag = 1;
		}
		if (flag == 0)
		{
			write_back_count = sector_count - free_sector;
			ssd->dram->buffer->write_miss_hit = ssd->dram->buffer->write_miss_hit + write_back_count;
			while (write_back_count>0)
			{
				sub_req = NULL;
				sub_req_state = ssd->dram->buffer->buffer_tail->stored;
				sub_req_size = size(ssd->dram->buffer->buffer_tail->stored);
				sub_req_lpn = ssd->dram->buffer->buffer_tail->group;
				sub_req = creat_sub_request(ssd, sub_req_lpn, sub_req_size, sub_req_state, req, WRITE);

				/**********************************************************************************
				*req不为空，表示这个insert2buffer函数是在buffer_management中调用，传递了request进来
				*req为空，表示这个函数是在process函数中处理一对多映射关系的读的时候，需要将这个读出
				*的数据加到buffer中，这可能产生实时的写回操作，需要将这个实时的写回操作的子请求挂在
				*这个读请求的总请求上
				***********************************************************************************/
				if (req != NULL)
				{
				}
				else
				{
					sub_req->next_subs = sub->next_subs;
					sub->next_subs = sub_req;
				}

				/*********************************************************************
				*写请求插入到了平衡二叉树，这时就要修改dram的buffer_sector_count；
				*维持平衡二叉树调用avlTreeDel()和AVL_TREENODE_FREE()函数；维持LRU算法；
				**********************************************************************/
				ssd->dram->buffer->buffer_sector_count = ssd->dram->buffer->buffer_sector_count - sub_req->size;
				pt = ssd->dram->buffer->buffer_tail;
				avlTreeDel(ssd->dram->buffer, (TREE_NODE *)pt);
				if (ssd->dram->buffer->buffer_head->LRU_link_next == NULL){
					ssd->dram->buffer->buffer_head = NULL;
					ssd->dram->buffer->buffer_tail = NULL;
				}
				else{
					ssd->dram->buffer->buffer_tail = ssd->dram->buffer->buffer_tail->LRU_link_pre;
					ssd->dram->buffer->buffer_tail->LRU_link_next = NULL;
				}
				pt->LRU_link_next = NULL;
				pt->LRU_link_pre = NULL;
				AVL_TREENODE_FREE(ssd->dram->buffer, (TREE_NODE *)pt);
				pt = NULL;

				write_back_count = write_back_count - sub_req->size;                            /*因为产生了实时写回操作，需要将主动写回操作区域增加*/
			}
		}

		/******************************************************************************
		*生成一个buffer node，根据这个页的情况分别赋值个各个成员，添加到队首和二叉树中
		*******************************************************************************/
		new_node = NULL;
		new_node = (struct buffer_group *)malloc(sizeof(struct buffer_group));
		alloc_assert(new_node, "buffer_group_node");
		memset(new_node, 0, sizeof(struct buffer_group));

		new_node->group = lpn;
		new_node->stored = state;
		new_node->dirty_clean = state;
		new_node->LRU_link_pre = NULL;
		new_node->LRU_link_next = ssd->dram->buffer->buffer_head;
		if (ssd->dram->buffer->buffer_head != NULL){
			ssd->dram->buffer->buffer_head->LRU_link_pre = new_node;
		}
		else{
			ssd->dram->buffer->buffer_tail = new_node;
		}
		ssd->dram->buffer->buffer_head = new_node;
		new_node->LRU_link_pre = NULL;
		avlTreeAdd(ssd->dram->buffer, (TREE_NODE *)new_node);
		ssd->dram->buffer->buffer_sector_count += sector_count;
	}
	/****************************************************************************************
	*在buffer中命中的情况
	*算然命中了，但是命中的只是lpn，有可能新来的写请求，只是需要写lpn这一page的某几个sub_page
	*这时有需要进一步的判断
	*****************************************************************************************/
	else
	{
		for (i = 0; i<ssd->parameter->subpage_page; i++)
		{
			/*************************************************************
			*判断state第i位是不是1
			*并且判断第i个sector是否存在buffer中，1表示存在，0表示不存在。
			**************************************************************/
			if ((state >> i) % 2 != 0)
			{
				lsn = lpn*ssd->parameter->subpage_page + i;
				hit_flag = 0;
				hit_flag = (buffer_node->stored)&(0x00000001 << i);

				if (hit_flag != 0)				                                          /*命中了，需要将该节点移到buffer的队首，并且将命中的lsn进行标记*/
				{
					active_region_flag = 1;                                             /*用来记录在这个buffer node中的lsn是否被命中，用于后面对阈值的判定*/

					if (req != NULL)
					{
						if (ssd->dram->buffer->buffer_head != buffer_node)
						{
							if (ssd->dram->buffer->buffer_tail == buffer_node)
							{
								ssd->dram->buffer->buffer_tail = buffer_node->LRU_link_pre;
								buffer_node->LRU_link_pre->LRU_link_next = NULL;
							}
							else if (buffer_node != ssd->dram->buffer->buffer_head)
							{
								buffer_node->LRU_link_pre->LRU_link_next = buffer_node->LRU_link_next;
								buffer_node->LRU_link_next->LRU_link_pre = buffer_node->LRU_link_pre;
							}
							buffer_node->LRU_link_next = ssd->dram->buffer->buffer_head;
							ssd->dram->buffer->buffer_head->LRU_link_pre = buffer_node;
							buffer_node->LRU_link_pre = NULL;
							ssd->dram->buffer->buffer_head = buffer_node;
						}
						ssd->dram->buffer->write_hit++;
						req->complete_lsn_count++;                                        /*关键 当在buffer中命中时 就用req->complete_lsn_count++表示往buffer中写了数据。*/
					}
					else
					{
					}
				}
				else
				{
					/************************************************************************************************************
					*该lsn没有命中，但是节点在buffer中，需要将这个lsn加到buffer的对应节点中
					*从buffer的末端找一个节点，将一个已经写回的lsn从节点中删除(如果找到的话)，更改这个节点的状态，同时将这个新的
					*lsn加到相应的buffer节点中，该节点可能在buffer头，不在的话，将其移到头部。如果没有找到已经写回的lsn，在buffer
					*节点找一个group整体写回，将这个子请求挂在这个请求上。可以提前挂在一个channel上。
					*第一步:将buffer队尾的已经写回的节点删除一个，为新的lsn腾出空间，这里需要修改队尾某节点的stored状态这里还需要
					*       增加，当没有可以之间删除的lsn时，需要产生新的写子请求，写回LRU最后的节点。
					*第二步:将新的lsn加到所述的buffer节点中。
					*************************************************************************************************************/
					ssd->dram->buffer->write_miss_hit++;

					if (ssd->dram->buffer->buffer_sector_count >= ssd->dram->buffer->max_buffer_sector)
					{
						if (buffer_node == ssd->dram->buffer->buffer_tail)                  /*如果命中的节点是buffer中最后一个节点，交换最后两个节点*/
						{
							pt = ssd->dram->buffer->buffer_tail->LRU_link_pre;
							ssd->dram->buffer->buffer_tail->LRU_link_pre = pt->LRU_link_pre;
							ssd->dram->buffer->buffer_tail->LRU_link_pre->LRU_link_next = ssd->dram->buffer->buffer_tail;
							ssd->dram->buffer->buffer_tail->LRU_link_next = pt;
							pt->LRU_link_next = NULL;
							pt->LRU_link_pre = ssd->dram->buffer->buffer_tail;
							ssd->dram->buffer->buffer_tail = pt;

						}
						sub_req = NULL;
						sub_req_state = ssd->dram->buffer->buffer_tail->stored;
						sub_req_size = size(ssd->dram->buffer->buffer_tail->stored);
						sub_req_lpn = ssd->dram->buffer->buffer_tail->group;
						sub_req = creat_sub_request(ssd, sub_req_lpn, sub_req_size, sub_req_state, req, WRITE);

						if (req != NULL)
						{

						}
						else if (req == NULL)
						{
							sub_req->next_subs = sub->next_subs;
							sub->next_subs = sub_req;
						}

						ssd->dram->buffer->buffer_sector_count = ssd->dram->buffer->buffer_sector_count - sub_req->size;
						pt = ssd->dram->buffer->buffer_tail;
						avlTreeDel(ssd->dram->buffer, (TREE_NODE *)pt);

						/************************************************************************/
						/* 改:  挂在了子请求，buffer的节点不应立即删除，						*/
						/*			需等到写回了之后才能删除									*/
						/************************************************************************/
						if (ssd->dram->buffer->buffer_head->LRU_link_next == NULL)
						{
							ssd->dram->buffer->buffer_head = NULL;
							ssd->dram->buffer->buffer_tail = NULL;
						}
						else{
							ssd->dram->buffer->buffer_tail = ssd->dram->buffer->buffer_tail->LRU_link_pre;
							ssd->dram->buffer->buffer_tail->LRU_link_next = NULL;
						}
						pt->LRU_link_next = NULL;
						pt->LRU_link_pre = NULL;
						AVL_TREENODE_FREE(ssd->dram->buffer, (TREE_NODE *)pt);
						pt = NULL;
					}

					/*第二步:将新的lsn加到所述的buffer节点中*/
					add_flag = 0x00000001 << (lsn%ssd->parameter->subpage_page);

					if (ssd->dram->buffer->buffer_head != buffer_node)                      /*如果该buffer节点不在buffer的队首，需要将这个节点提到队首*/
					{
						if (ssd->dram->buffer->buffer_tail == buffer_node)
						{
							buffer_node->LRU_link_pre->LRU_link_next = NULL;
							ssd->dram->buffer->buffer_tail = buffer_node->LRU_link_pre;
						}
						else
						{
							buffer_node->LRU_link_pre->LRU_link_next = buffer_node->LRU_link_next;
							buffer_node->LRU_link_next->LRU_link_pre = buffer_node->LRU_link_pre;
						}
						buffer_node->LRU_link_next = ssd->dram->buffer->buffer_head;
						ssd->dram->buffer->buffer_head->LRU_link_pre = buffer_node;
						buffer_node->LRU_link_pre = NULL;
						ssd->dram->buffer->buffer_head = buffer_node;
					}
					buffer_node->stored = buffer_node->stored | add_flag;
					buffer_node->dirty_clean = buffer_node->dirty_clean | add_flag;
					ssd->dram->buffer->buffer_sector_count++;
				}

			}
		}
	}

	return ssd;
}

/**********************************************************************************
*读请求分配子请求函数，这里只处理读请求，写请求已经在buffer_management()函数中处理了
*根据请求队列和buffer命中的检查，将每个请求分解成子请求，将子请求队列挂在channel上，
*不同的channel有自己的子请求队列
**********************************************************************************/

struct ssd_info *distribute(struct ssd_info *ssd)
{
	unsigned int start, end, first_lsn, last_lsn, lpn, flag = 0, flag_attached = 0, full_page;
	unsigned int j, k, sub_size;
	int i = 0;
	struct request *req;
	struct sub_request *sub;
	int* complt;

#ifdef DEBUG
	printf("enter distribute,  current time:%I64u\n", ssd->current_time);
#endif
	full_page = ~(0xffffffff << ssd->parameter->subpage_page);

	req = ssd->request_tail;
	if (req->response_time != 0){
		return ssd;
	}
	if (req->operation == WRITE)
	{
		return ssd;
	}

	if (req != NULL)
	{
		if (req->distri_flag == 0)
		{
			//如果还有一些读请求需要处理
			if (req->complete_lsn_count != ssd->request_tail->size)
			{
				first_lsn = req->lsn;
				last_lsn = first_lsn + req->size;
				complt = req->need_distr_flag;
				start = first_lsn - first_lsn % ssd->parameter->subpage_page;
				end = (last_lsn / ssd->parameter->subpage_page + 1) * ssd->parameter->subpage_page;
				i = (end - start) / 32;

				while (i >= 0)
				{
					/*************************************************************************************
					*一个32位的整型数据的每一位代表一个子页，32/ssd->parameter->subpage_page就表示有多少页，
					*这里的每一页的状态都存放在了 req->need_distr_flag中，也就是complt中，通过比较complt的
					*每一项与full_page，就可以知道，这一页是否处理完成。如果没处理完成则通过creat_sub_request
					函数创建子请求。
					*************************************************************************************/
					for (j = 0; j<32 / ssd->parameter->subpage_page; j++)
					{
						k = (complt[((end - start) / 32 - i)] >> (ssd->parameter->subpage_page*j)) & full_page;
						if (k != 0)
						{
							lpn = start / ssd->parameter->subpage_page + ((end - start) / 32 - i) * 32 / ssd->parameter->subpage_page + j;
							sub_size = transfer_size(ssd, k, lpn, req);
							if (sub_size == 0)
							{
								continue;
							}
							else
							{
								sub = creat_sub_request(ssd, lpn, sub_size, 0, req, req->operation);
							}
						}
					}
					i = i - 1;
				}

			}
			else
			{
				req->begin_time = ssd->current_time;
				req->response_time = ssd->current_time + 1000;
			}

		}
	}
	return ssd;
}


/*********************************************************************************************
*no_buffer_distribute()函数是处理当ssd没有dram的时候，
*这是读写请求就不必再需要在buffer里面寻找，直接利用creat_sub_request()函数创建子请求，再处理。
*********************************************************************************************/
struct ssd_info *no_buffer_distribute(struct ssd_info *ssd)
{
	unsigned int lsn, lpn, last_lpn, first_lpn, complete_flag = 0, state;
	unsigned int flag = 0, flag1 = 1, active_region_flag = 0;           //to indicate the lsn is hitted or not
	struct request *req = NULL;
	struct sub_request *sub = NULL, *sub_r = NULL, *update = NULL;
	struct local *loc = NULL;
	struct channel_info *p_ch = NULL;


	unsigned int mask = 0;
	unsigned int offset1 = 0, offset2 = 0;
	unsigned int sub_size = 0;
	unsigned int sub_state = 0;

	ssd->dram->current_time = ssd->current_time;
	req = ssd->request_tail;
	lsn = req->lsn;
	lpn = req->lsn / ssd->parameter->subpage_page;
	last_lpn = (req->lsn + req->size - 1) / ssd->parameter->subpage_page;
	first_lpn = req->lsn / ssd->parameter->subpage_page;

	if (req->operation == READ)
	{
		while (lpn <= last_lpn)
		{
			sub_state = (ssd->dram->map->map_entry[lpn].state & 0x7fffffff);
			sub_size = size(sub_state);
			sub = creat_sub_request(ssd, lpn, sub_size, sub_state, req, req->operation);
			lpn++;
		}
	}
	else if (req->operation == WRITE)
	{
		while (lpn <= last_lpn)
		{
			mask = ~(0xffffffff << (ssd->parameter->subpage_page));
			state = mask;
			if (lpn == first_lpn)
			{
				offset1 = ssd->parameter->subpage_page - ((lpn + 1)*ssd->parameter->subpage_page - req->lsn);
				state = state&(0xffffffff << offset1);
			}
			if (lpn == last_lpn)
			{
				offset2 = ssd->parameter->subpage_page - ((lpn + 1)*ssd->parameter->subpage_page - (req->lsn + req->size));
				state = state&(~(0xffffffff << offset2));
			}
			sub_size = size(state);

			sub = creat_sub_request(ssd, lpn, sub_size, state, req, req->operation);
			lpn++;
		}
	}

	return ssd;
}

/*********************************************************
*transfer_size()函数的作用就是计算出子请求的需要处理的size
*函数中单独处理了first_lpn，last_lpn这两个特别情况，因为这
*两种情况下很有可能不是处理一整页而是处理一页的一部分，因
*为lsn有可能不是一页的第一个子页。
*********************************************************/
unsigned int transfer_size(struct ssd_info *ssd, int need_distribute, unsigned int lpn, struct request *req)
{
	unsigned int first_lpn, last_lpn, state, trans_size;
	unsigned int mask = 0, offset1 = 0, offset2 = 0;

	first_lpn = req->lsn / ssd->parameter->subpage_page;
	last_lpn = (req->lsn + req->size - 1) / ssd->parameter->subpage_page;

	mask = ~(0xffffffff << (ssd->parameter->subpage_page));
	state = mask;
	if (lpn == first_lpn)
	{
		offset1 = ssd->parameter->subpage_page - ((lpn + 1)*ssd->parameter->subpage_page - req->lsn);
		state = state&(0xffffffff << offset1);
	}
	if (lpn == last_lpn)
	{
		offset2 = ssd->parameter->subpage_page - ((lpn + 1)*ssd->parameter->subpage_page - (req->lsn + req->size));
		state = state&(~(0xffffffff << offset2));
	}

	trans_size = size(state&need_distribute);

	return trans_size;
}



/***********************************************************************************
*根据每一页的状态计算出每一需要处理的子页的数目，也就是一个子请求需要处理的子页的页数
************************************************************************************/
unsigned int size(unsigned int stored)
{
	unsigned int i, total = 0, mask = 0x80000000;

#ifdef DEBUG
	printf("enter size\n");
#endif
	for (i = 1; i <= 32; i++)
	{
		if (stored & mask) total++;
		stored <<= 1;
	}
#ifdef DEBUG
	printf("leave size\n");
#endif
	return total;
}


/**********************************************
*这个函数的功能是根据lpn，size，state创建子请求
**********************************************/
struct sub_request * creat_sub_request(struct ssd_info * ssd, unsigned int lpn, int size, unsigned int state, struct request * req, unsigned int operation)
{
	struct sub_request* sub = NULL, *sub_r = NULL;
	struct channel_info * p_ch = NULL;
	struct local * loc = NULL;
	unsigned int flag = 0;

	sub = (struct sub_request*)malloc(sizeof(struct sub_request));                        /*申请一个子请求的结构*/
	alloc_assert(sub, "sub_request");
	memset(sub, 0, sizeof(struct sub_request));

	if (sub == NULL)
	{
		return NULL;
	}
	sub->location = NULL;
	sub->next_node = NULL;
	sub->next_subs = NULL;
	sub->update = NULL;

	if (req != NULL)
	{
		sub->next_subs = req->subs;
		req->subs = sub;
	}

	/*************************************************************************************
	*在读操作的情况下，有一点非常重要就是要预先判断读子请求队列中是否有与这个子请求相同的，
	*有的话，新子请求就不必再执行了，将新的子请求直接赋为完成
	**************************************************************************************/
	if (operation == READ)
	{
		loc = find_location(ssd, ssd->dram->map->map_entry[lpn].pn);
		sub->location = loc;
		sub->begin_time = ssd->current_time;
		sub->current_state = SR_WAIT;
		sub->current_time = 0x7fffffffffffffff;
		sub->next_state = SR_R_C_A_TRANSFER;
		sub->next_state_predict_time = 0x7fffffffffffffff;
		sub->lpn = lpn;
		sub->size = size;                                                               /*需要计算出该子请求的请求大小*/

		p_ch = &ssd->channel_head[loc->channel];
		sub->ppn = ssd->dram->map->map_entry[lpn].pn;
		sub->operation = READ;
		sub->state = (ssd->dram->map->map_entry[lpn].state & 0x7fffffff);
		sub_r = p_ch->subs_r_head;                                                      /*一下几行包括flag用于判断该读子请求队列中是否有与这个子请求相同的，有的话，将新的子请求直接赋为完成*/
		flag = 0;
		while (sub_r != NULL)
		{
			if (sub_r->ppn == sub->ppn)
			{
				flag = 1;
				break;
			}
			sub_r = sub_r->next_node;
		}
		if (flag == 0)
		{
			if (p_ch->subs_r_tail != NULL)
			{
				p_ch->subs_r_tail->next_node = sub;
				p_ch->subs_r_tail = sub;
			}
			else
			{
				p_ch->subs_r_head = sub;
				p_ch->subs_r_tail = sub;
			}
		}
		else
		{
			sub->current_state = SR_R_DATA_TRANSFER;
			sub->current_time = ssd->current_time;
			sub->next_state = SR_COMPLETE;
			sub->next_state_predict_time = ssd->current_time + 1000;
			sub->complete_time = ssd->current_time + 1000;
		}
	}
	/*************************************************************************************
	*写请求的情况下，就需要利用到函数allocate_location(ssd ,sub)来处理静态分配和动态分配了
	**************************************************************************************/
	else if (operation == WRITE)
	{
		sub->ppn = 0;
		sub->operation = WRITE;
		sub->location = (struct local *)malloc(sizeof(struct local));
		alloc_assert(sub->location, "sub->location");
		memset(sub->location, 0, sizeof(struct local));

		sub->current_state = SR_WAIT;
		sub->current_time = ssd->current_time;
		sub->lpn = lpn;
		sub->size = size;
		sub->state = state;
		sub->begin_time = ssd->current_time;

		if (allocate_location(ssd, sub) == ERROR)
		{
			free(sub->location);
			sub->location = NULL;
			free(sub);
			sub = NULL;
			return NULL;
		}

	}
	else
	{
		free(sub->location);
		sub->location = NULL;
		free(sub);
		sub = NULL;
		printf("\nERROR ! Unexpected command.\n");
		return NULL;
	}

	return sub;
}

/**********************
*这个函数只作用于写请求
***********************/
Status allocate_location(struct ssd_info * ssd, struct sub_request *sub_req)
{
	struct sub_request * update = NULL;
	unsigned int channel_num = 0, chip_num = 0, die_num = 0, plane_num = 0;
	struct local *location = NULL;

	channel_num = ssd->parameter->channel_number;
	chip_num = ssd->parameter->chip_channel[0];
	die_num = ssd->parameter->die_chip;
	plane_num = ssd->parameter->plane_die;


	if (ssd->parameter->allocation_scheme == 0)                                          /*动态分配的情况*/
	{
		/******************************************************************
		* 在动态分配中，因为页的更新操作使用不了copyback操作，
		*需要产生一个读请求，并且只有这个读请求完成后才能进行这个页的写操作
		*******************************************************************/
		if (ssd->dram->map->map_entry[sub_req->lpn].state != 0)
		{
			if ((sub_req->state&ssd->dram->map->map_entry[sub_req->lpn].state) != ssd->dram->map->map_entry[sub_req->lpn].state)
			{
				ssd->read_count++;
				ssd->update_read_count++;

				update = (struct sub_request *)malloc(sizeof(struct sub_request));
				alloc_assert(update, "update");
				memset(update, 0, sizeof(struct sub_request));

				if (update == NULL)
				{
					return ERROR;
				}
				update->location = NULL;
				update->next_node = NULL;
				update->next_subs = NULL;
				update->update = NULL;
				location = find_location(ssd, ssd->dram->map->map_entry[sub_req->lpn].pn);
				update->location = location;
				update->begin_time = ssd->current_time;
				update->current_state = SR_WAIT;
				update->current_time = 0x7fffffffffffffff;
				update->next_state = SR_R_C_A_TRANSFER;
				update->next_state_predict_time = 0x7fffffffffffffff;
				update->lpn = sub_req->lpn;
				update->state = ((ssd->dram->map->map_entry[sub_req->lpn].state^sub_req->state) & 0x7fffffff);
				update->size = size(update->state);
				update->ppn = ssd->dram->map->map_entry[sub_req->lpn].pn;
				update->operation = READ;

				if (ssd->channel_head[location->channel].subs_r_tail != NULL)            /*产生新的读请求，并且挂到channel的subs_r_tail队列尾*/
				{
					ssd->channel_head[location->channel].subs_r_tail->next_node = update;
					ssd->channel_head[location->channel].subs_r_tail = update;
				}
				else
				{
					ssd->channel_head[location->channel].subs_r_tail = update;
					ssd->channel_head[location->channel].subs_r_head = update;
				}
			}
		}
		/***************************************
		*一下是动态分配的几种情况
		*0：全动态分配
		*1：表示channel定package，die，plane动态
		****************************************/
		//只考虑全动态分配
		switch (ssd->parameter->dynamic_allocation)
		{
		case 0:
		{
			sub_req->location->channel = -1;
			sub_req->location->chip = -1;
			sub_req->location->die = -1;
			sub_req->location->plane = -1;
			sub_req->location->block = -1;
			sub_req->location->page = -1;

			if (ssd->subs_w_tail != NULL)
			{
				ssd->subs_w_tail->next_node = sub_req;
				ssd->subs_w_tail = sub_req;
			}
			else
			{
				ssd->subs_w_tail = sub_req;
				ssd->subs_w_head = sub_req;
			}

			if (update != NULL)
			{
				sub_req->update = update;
			}

			break;
		}
		}
	}
	return SUCCESS;
}