# MIT 6.828 - 麻省理工操作系统内核开发课程

## 前言 
这是麻省理工的操作系统内核开发课程 [MIT 6.828](https://pdos.csail.mit.edu/6.828/2018/)，在这个项目中，你会真正的用 C 语言去写操作系统内核的代码。

目前已有不少前辈分享了自己的成果，先在此表达感谢与敬意。那么我的项目能给读者带来什么呢？以下是关于我的代码的介绍：

1. **Lab 1~5 全部代码练习的答案，通过官方所有的打分测试**
2. **对一些关键机制进行详细的讲解**
3. **对代码的一些分析和思考**
4. **记录下实践过程中一些关键的 Bug**
5. **丰富的注释**
6. **git 版本控制**，自 Lab 2 后将练习按顺序拆分为多个 commit，方便查阅相关练习代码

总结来说，与其提供一份“干净的”最终结果代码，我希望记录下在实践过程中，遇到的问题、个人的思考以及学到的知识，让读者明白“为什么在这里我要这么写”。
因为我本科时主修的并不是计算机，在自学的过程中，本人深感“即使看到了源码也觉得难以理解”的痛苦，所以希望在完成这个项目的过程中，能提供给读者更多的东西。
当然，由于本人水平有限，分享出来的东西不一定正确，还请读者海涵和辨别，欢迎各位指出问题！

接下来我将分别介绍本项目的前置知识点、使用指南、注意事项、环境配置、版权声明以及参考资源。


## 前置知识点

1. **完成大学本科的操作系统课程学习，至少同步学习。**  
   本项目不会解释一些基础的概念，比如页表，进程，也不会解释一些基础的操作，比如如何将虚拟地址转换为物理地址。
   
2. **完成计算机组成原理以及数据结构的本科课程。**
   
3. **熟练使用 C 语言，特别是指针和类型转换。**
   
4. **对基础的汇编语言有一定的了解。** 因为有些实验涉及到汇编语言（现学也不难）。
   
5. **一颗勇敢的心。**


## 使用指南

一步一步跟着官方的指导完成即可，官方的指导链接如下：

- [Lab 1: PC Bootstrap and GCC Calling Conventions](https://pdos.csail.mit.edu/6.828/2018/labs/lab1/)
- [Lab 2: Memory Management](https://pdos.csail.mit.edu/6.828/2018/labs/lab2/)
- [Lab 3: User Environments](https://pdos.csail.mit.edu/6.828/2018/labs/lab3/)
- [Lab 4: Preemptive Multitasking](https://pdos.csail.mit.edu/6.828/2018/labs/lab4/)
- [Lab 5: File system, Spawn, and Shell](https://pdos.csail.mit.edu/6.828/2018/labs/lab5/)

大体可沿着“看指导 → 看代码 → 写代码”这条主线进行。一些优质的参考资源我会在之后列出。

特别的，对于 Lab 1，主要是有关操作系统引导的知识，由于涉及到很多硬件的内容（例如：实模式与保护模式、各种寄存器的作用），我个人建议还是系统地去学习一下相关知识，
这里推荐可以看看《Linux源码趣读》这本书关于引导操作系统部分的章节。如果你对这方面的细节不感兴趣，没关系，只需要知道基本概念：BIOS、磁盘引导扇区、
为什么需要引导操作系统，以及大致的流程即可。Lab 1 基本不需要我们编写代码。


## 注意事项

正如前言所说，我并不打算只提供一份干净的代码，这意味着你会在某些commit中看到明显或不那么明显的Bug（这些Bug会在之后的commit中得到修复）。
本人对于commit的依据是“能通过当前练习的打分程序（如果有补丁的话，以补丁为准）”，能通过当前的打分程序并不意味着能通过后续所有的打分程序（就算能通过所有的打分程序，
也并不意味着没有Bug，谁能写出没有Bug的代码呢？），所以如果读者觉得我的代码写错了，请大胆质疑！同理，我的一些注释只是如实记录了“在这个时候”的理解，
然而”在后来的某个时刻”我又觉得之前的理解不太准确就把它改掉了，这是很正常的，所以还请读者辨别地看待，独立思考。


## 环境配置

**1. 配置Linux环境。** 如果你是Windows电脑，需要配置一个Liunx的运行环境，你可以选择用VirtualBox等软件,但是我个人建议用WSL，VirtualBox的好处是有图形界面，
但是我相信你会更喜欢WSL+Ubuntu+vscode的组合。至于怎么安装WSL+Ubuntu，网上有很多教程。（本人使用Ubuntu 22.04.3 LTS）

**2. 配置工具链。** 官方链接：[https://pdos.csail.mit.edu/6.828/2018/tools.html](https://pdos.csail.mit.edu/6.828/2018/tools.html)。怎么知道工具链是否装好了呢？
按照官方的Test Your Compiler Toolchain步骤做一下即可。
如果没装好的话，就是执行经典的`sudo apt-get install`了。如果你对Liunx指令完全没有概念，也不用担心，让gpt教一下你，有什么不懂的问问gpt，很快就能学会了。
注意你需要安装一个32位的支持库，也就是`sudo apt-get install gcc-multilib`，不然到时可能会报`undefined reference to '__udivdi3'，undefined reference to '__umoddi3'`这个错误。
除此之外，你还需要安装一下git，熟悉一下最基本的git操作，以后做实验会用到。

**3. 安装QEMU。** Mit 6.828的代码只能运行在“老一点”的硬件上，是没办法直接在我们现代的硬件上直接运行的，所以需要提供一个虚拟的硬件环境，让它在这上面跑，qemu就充当了这个作用。
参考[https://pdos.csail.mit.edu/6.828/2018/tools.html](https://pdos.csail.mit.edu/6.828/2018/tools.html)的QEMU Emulator部分安装。
强烈建议用官方的补丁版本。
官方补丁版在`make`的时候可能会报错
```
/usr/bin/ld: qga/commands-posix.o: in function `dev_major_minor':
/home/qemu/qga/commands-posix.c:633: undefined reference to 'major' 
/home/qemu/qga/commands-posix.c:634: undefined reference to `minor'
collect2: error: ld returned 1 exit status
```
先看看是不是漏安装包了，如果工具链配置没问题，还是有这个问题的话，那么可以手动在qga/commands-posix.c中补充上
```
#define major(dev) (((dev) >> 8) & 0xfff)
#define minor(dev) ((dev) & 0xff)
```
 
**4. 下载项目代码并编译运行。** `git clone https://pdos.csail.mit.edu/6.828/2018/jos.git lab`。clone完成后进入lab目录，`make`一下，然后`make qemu-nox`或者`make qemu`。
你应该能看到以下几行：
```
Weclome to the JOS kernel monitor!
Type ‘help’ for a list of commands.
K>
```

**5. 安装vscode。** 网上有教程。如果安装成功了，进入lab目录，输入`code .`（注意这里有个”.”，表示当前目录），应该会进入到vscode的界面。
试着用vscode里面的终端运行一下项目代码，正确运行后，环境配置也就完成了！


## 版权声明

本项目练习代码中的几乎所有代码均由我个人独立完成，参考别人代码部分的对应链接记录在Debug_record.txt和Process_schedule_refactor.txt中。欢迎学习，分享，改进等善意行为，禁止剽窃。


## 参考资源

1. [MIT6.828-神级OS课程-要是早遇到，我还会是这种 five 系列](https://zhuanlan.zhihu.com/p/74028717)
2. [fatsheep9146 MIT 6.828 JOS 操作系统学习笔记](https://www.cnblogs.com/fatsheep9146/category/769143.html)
3. 《Linux源码趣读》，作者：闪客，出版社：电子工业出版社
