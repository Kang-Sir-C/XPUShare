package scheduler

/** Filter **/
// filter node according to the node and its gpu model
func (kss *XPUShareScheduler) filterNode(nodeName, model string, request float64, memory int64) (bool, float64, int64) {
	kss.ksl.Debugf("[Filter] filterNode: node %v with gpu model %v", nodeName, model)

	ok := false
	available := 0.0
	freeMemory := int64(0)
	//freeList := kss.cellFreeList[model]

	////////////////////////////////////////
	// -------------------------------------------------------------
	// 🔴 修复/调试逻辑开始：确定正确的 CellFreeList 查找键
	// -------------------------------------------------------------
	
	// 假设叶子 Cell 模型 ("Iluvatar-BI-V150") 不在 CellFreeList 的顶层键中，
	// 实际键是根 Cell 类型 ("NODE-16-V150")。
	// 这里我们使用 Pod 请求的 'model' 作为查找键。
	lookupKey := model 
	
	// 如果 Pod 请求的型号不是 CellFreeList 的顶层键，但已知根 Cell 是唯一的，
	// 则尝试使用已知的根 Cell 类型作为键。
	// 硬编码的根 Cell 类型（根据您的配置推断）：
	rootCellType := "NODE-16-V150" 

	// 检查 Pod 请求的型号是否在 CellFreeList 中
	freeList, found := kss.cellFreeList[lookupKey]

	if !found {
		// 如果 Pod 请求的叶子型号找不到，则尝试用根 Cell 型号查找。
		freeList, found = kss.cellFreeList[rootCellType]
		if found {
			lookupKey = rootCellType

		}
	}
	
	if !found {
		return false, 0, 0
	}
	// -------------------------------------------------------------
	// 🔴 修复/调试逻辑结束
	// -------------------------------------------------------------




	for _, cellList := range freeList {
		for _, cell := range cellList {
			kss.ksl.Debugf("[Filter] filterNode: cell %v", cell)
			fit, currentAvailable, currentMemory := kss.checkCellResource(cell, nodeName, request, memory)
			ok = ok || fit
			available += currentAvailable
			freeMemory += currentMemory
			// prune
			if ok {
				return ok, available, freeMemory
			}
		}
	}

	return ok, available, freeMemory
}

// check if the gpu resource in the node can fit the pod requirement
// and calculate its free resource in the specified gpu model
func (kss *XPUShareScheduler) checkCellResource(cell *Cell, nodeName string, request float64, memory int64) (bool, float64, int64) {
	s := NewStack()

	node := cell.node
	// this cell does not need to check
	if node != nodeName && node != "" {
		return false, 0.0, 0
	}

	if cell.healthy {
		s.Push(cell)
	}

	multiGPU := request > 1.0
	availableWholeCell := float64(0.0)
	freeMemory := int64(0)

	if multiGPU {
		for s.Len() > 0 {
			current := s.Pop()
			kss.ksl.Debugf("[Filter] Check cell resource (multiGPU): %+v", current)

			if current.node == nodeName && current.isNode && current.healthy {


				availableWholeCell += current.availableWholeCell
				freeMemory += current.freeMemory
				kss.ksl.Debugf("[Filter] node %v, availableWholeCell %v request-> %v, freeMemory %v reqeust-> %v", nodeName, availableWholeCell, request, freeMemory, memory)
				if availableWholeCell >= request && freeMemory >= memory {
					return true, availableWholeCell, freeMemory
				}
			}

			child := current.child
			if child == nil {
				continue
			}
			if current.higherThanNode && current.healthy {
				for i := range child {
					node := child[i].node
					if (node == nodeName || node == "") && child[i].healthy {
		
						s.Push(child[i])
					}
				}
			}
		}
	} else {
		for s.Len() > 0 {
			current := s.Pop()
			if current.node == nodeName && current.healthy && current.level == 1 {

				if current.available >= request && current.freeMemory >= memory {
					return true, current.available, current.freeMemory
				}
			}
			child := current.child
			if child == nil {
				continue
			}
			for i := range child {
				node := child[i].node
				if (node == nodeName || node == "") && child[i].healthy {
					
					s.Push(child[i])
				}
			}
		}
	}
	if multiGPU {
		return false, availableWholeCell, freeMemory
	} else {
		return false, 0, 0
	}
}

/*
// filter node according to the node and its gpu model
func (kss *XPUShareScheduler) filterNode(nodeName, model string, request float64, memory int64) (bool, float64, int64) {
	kss.ksl.Debugf("[Filter] filterNode: node %v with gpu model %v", nodeName, model)

	ok := false
	available := 0.0
	freeMemory := int64(0)
	freeList := kss.cellFreeList[model]

	for _, cellList := range freeList {
		for _, cell := range cellList {
			kss.ksl.Debugf("[Filter] filterNode: cell %v", cell)
			fit, currentAvailable, currentMemory := kss.checkCellResource(cell, nodeName, request, memory)
			ok = ok || fit
			available += currentAvailable
			freeMemory += currentMemory
			// prune
			if ok {
				return ok, available, freeMemory
			}
		}
	}

	return ok, available, freeMemory
}

func (kss *XPUShareScheduler) checkCellResource(cell *Cell, nodeName string, request float64, memory int64) (bool, float64, int64) {
	s := NewStack()

	node := cell.node
	// this cell does not need to check
	if node != nodeName && node != "" {
		return false, 0.0, 0
	}

	if cell.healthy {
		s.Push(cell)
	}

	multiGPU := request > 1.0
	availableWholeCell := float64(0.0)
	freeMemory := int64(0)
	for s.Len() > 0 {
		current := s.Pop()
		kss.ksl.Debugf("[Filter] Check resource cell: %+v", current)

		if current.node == nodeName && current.healthy {
			// only need whole gpu
			if multiGPU {
				availableWholeCell += current.availableWholeCell
				freeMemory += current.freeMemory
				kss.ksl.Debugf("[Filter] node %v, availableWholeCell %v request-> %v, freeMemory %v reqeust-> %v", nodeName, availableWholeCell, request, freeMemory, memory)
				if availableWholeCell >= request && freeMemory >= memory {
					return true, availableWholeCell, freeMemory
				}
			} else {
				if current.level == 1 && current.available >= request && current.freeMemory >= memory {
					return true, current.available, current.freeMemory
				}
			}
		}

		child := current.child
		if child == nil {
			continue
		}

		for i := range child {
			node := child[i].node
			if (node == nodeName || node == "") && child[i].healthy {
				kss.ksl.Debugf("[Filter] Check resource child: %+v", child[i])
				s.Push(child[i])
			}
		}
	}

	if multiGPU {
		return false, availableWholeCell, freeMemory
	} else {
		return false, 0, 0
	}

}
*/
