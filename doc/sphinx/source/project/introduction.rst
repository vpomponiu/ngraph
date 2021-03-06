.. introduction:

############
Introduction
############

Deep Learning (DL) computational performance is critical for scientists and engineers applying deep learning techniques to many challenges in healthcare, commerce, autonomous driving, and other domains. While they can leverage 


Motivations
===========

Kernel libraries do not support graph-level optimizations
---------------------------------------------------------

A framework designed for training using GPUs requires integration with a kernel 
library unique to that vendor's hardware. For example, after integration, a 
kernel library can run operations that it is "familar" with optimally; however, 
the graph itself within any larger :term:`NN` won't be optimal.

.. _figure-A:

.. figure:: ../graphics/framework-to-kernel-lib.png
   :width: 555px
   :alt: 

   Figure A: Lack of graph-level optimization makes framework-to-kernel library
   integration enormously inefficient. The computation graph above represents 
   the computation: "A plus B times C".


.. _figure-B:

.. figure:: ../graphics/framework-to-graph-opt.png
   :width: 555px
   :alt: 

   Figure B: Notice that an operation on the constant B (in this case a ``Broadcast``) 
   can be done at compile time. This is an example of constant folding, and it 
   is not available to a device-based kernel library.   


.. _figure-C:

.. figure:: ../graphics/ngraph-algebraic-simp.png
   :width: 555px
   :alt: 

   Figure C: Finally notice that the constant has value "zero" thus the add is an 
   *identity* operation and can be eliminated. This is an example of **Algebraic 
   simplification**, and it is not available to a device-based kernel library.


After the two graph-level optimizations above (**Algebraic Simplification** and 
**Constant Folding**),  we now have an optimal graph: A times C. Again, kernel 
libraries do not support this type of optimization. Although each implementation 
can be done individually, it will eventually yield an "exploding" number of 
kernels the larger and more complex an :abbr:`NN (Neural Network)` becomes. For 
some insight on why this happens, see the next section. 


Too Many Kernels to write
-------------------------

A typical network is constructed using some kind of language-based API, which 
translates the network or :abbr:`DL (Deep Learning)` model (statically or 
dynamically) into serialized graphs. Those graphs can then passed through a 
compilation process (the *Graph optimization or compilation* step in 
*Figure D* below), where various graph-level optimizations, like constant folding 
or fusion can happen. These processes require unique vendor-provided libraries 
to communicate with a driver (possibly through OpenCL\*, CUDA\*, or SYCL\*), to 
compile and execute an implementation (kernel) for a specific 
:abbr:`Instruction Set Architecture (ISA)`, or :term:`ISA`.

Illustrated below is a simplified DL stack, showing relative complexity of 
each component. Note that optimizing for any one on its own usually requires 
engineering expertise that can be highly specialized to that component, and that 
the terms have been simplified for illustrative purposes. 

.. _figure-D:

.. figure:: ../graphics/components-dl-stack.png
   :width: 700px
   :alt: A simplified DL stack

   Figure D: Components of a DL stack, simplified for illustrative purposes.

There are many deep learning frameworks, each with its own strengths and user 
bases. A setup that is common to many DL practitioners is shown in the 
illustration below.

.. _figure-E:

.. figure:: ../graphics/a-common-stack.png
   :width: 700px
   :alt: A common implementation

   Figure E: A commonly-implemented stack uses TensorFlow\* as the frontend. 
   The input is either optimized via Grappler, or executed directly via TensorFlow. 
   In either case, when targeting an Nvidia\* GPU, cuDNN is called to select an 
   optimal kernel for the operation; cuDNN then relies on CUDA\* or direct access 
   to run code on the target; in this toy example, the target is a V100.

A natural result of this approach is that the framework-level integration of 
kernel libraries does not scale. Rather, each individual framework must be 
manually integrated with each hardware-specific kernel library. Each integration 
is unique to the framework and its set of deep learning operators, its view on 
memory layout, its feature set, etc. Each of these connections, then, represents 
significant work for what will ultimately be a brittle setup that is enormously 
expensive to maintain.    

.. _figure-F:

.. figure:: ../graphics/dl-current-state.png
   :width: 700px
   :alt: Scalability matters

   Figure F: The number of kernels necessary to achieve optimal performance is 
   bounded by the product of the number of chip designs one wishes to support, 
   the number of data types supported, the number of operations, and the 
   cardinality of each parameter for each operation.

In the past, this upper bound was quite limited; however, since the industry is 
shifting toward a more diverse future in terms of deep learning hardware, the 
number of distinct kernels is exploding and will continue to explode.


Get the best of both worlds
---------------------------

Integrating a framework on nGraph can be an attractive option for hardware 
companies trying to design their own deep learning hardware or network architecture. 
Framework integration is non-trivial amount of work, and nGraph automatically 
does much of the heavy lifting. Furthermore, PlaidML can provide a wide range of 
hardware coverage and optimization automatically. Any hardware that supports 
LLVM, OpenCL, OpenGL, CUDA or Metal can be supported automatically with PlaidML 
and nGraph.  

.. _figure-G:

.. figure:: ../graphics/graph-compilers-at-a-glance.png
   :width: 700px
   :alt: Overview of various graph and tensor compilers.

   Figure G: Overview of various graph and tensor compilers.


.. _figure-H:

.. figure:: ../graphics/tensor-compilers-at-a-glance.png
   :width: 700px
   :alt: A closer look at tensor compilers.

   Figure H: A closer look at tensor compilers.


Other notable efforts
----------------------

A few other notable efforts in compiler projects include: 

* **TVM** https://github.com/dmlc/tvm
* **XLA** https://developers.googleblog.com/2017/03/xla-tensorflow-compiled.html
* **Glow** https://arxiv.org/pdf/1805.00907.pdf 




