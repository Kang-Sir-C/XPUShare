package scheduler

import (
	"sort"
	"strings"

	"github.com/sirupsen/logrus"
)

type (
	CellState string
	CellList  []*Cell
	// maps each level to cellList
	LevelCellList map[int]CellList
)

const (
	CellFree   CellState = "FREE"
	CellFilled CellState = "FILLED"

	lowestLevel int = 1
)

func appendCellList(cl1, cl2 CellList) CellList {
	// for _, cell := range cl2 {
	// 	cl1 = append(cl1, cell)
	// }
	cl1 = append(cl1, cl2...)
	return cl1
}

// internal structure to build the cell elements
// preprocess the information about the cell
type cellElement struct {
	cellType        string  // cell types
	level           int     // cell level, leaf cell is 1
	priority        int32   // cell priority
	childCellNumber float64 // how many child cells in the current cell
	childCellType   string  // child cell type
	leafCellNumber  float64 // how many leaf cells in the current cell
	leafCellType    string  // leaf cell type
	isNode          bool    // cell types is a node cell
	isMultiNodes    bool    // cell type is a multiple cell
}

func (kss *XPUShareScheduler) buildCellChains(cellTypes map[string]CellTypeSpec) map[string]*cellElement {
	cellElements := map[string]*cellElement{}
	typesToBuild := make([]string, 0, len(cellTypes))
	// 🔴 DEBUG DUMP 1: 打印所有原始 Cell Type 键
    kss.ksl.Errorf("[DEBUG FINAL] RAW CONFIG CELL TYPES START")
    for cellType := range cellTypes {
        typesToBuild = append(typesToBuild, cellType)
        kss.ksl.Errorf("[DEBUG FINAL]   RAW TYPE: >>>%s<<< (Length: %d)", cellType, len(cellType))
    }
    kss.ksl.Errorf("[DEBUG FINAL] RAW CONFIG CELL TYPES END")

	//for cellType := range cellTypes {
	//	kss.addCell(cellType, cellTypes, cellElements, 1)
	//}
	// 修复：确保所有 Cell 类型都被处理，特别是 NODE-16-V150
	for _, cellType := range typesToBuild {
		kss.addCell(cellType, cellTypes, cellElements, 1)
	}
	// 检查是否有任何元素被创建
    if len(cellElements) == 0 {
        kss.ksl.Errorf("[FATAL BUILD] buildCellChains failed to produce any cell element. Check cellTypes definition.")
    }

	// 🔴 DEBUG DUMP 2: 打印所有构建完成的 Cell Element 键
    kss.ksl.Errorf("[DEBUG FINAL] BUILT CELL ELEMENTS START")
    for key, ce := range cellElements {
        kss.ksl.Errorf("[DEBUG FINAL]   BUILT KEY: >>>%s<<< LeafType: >>>%s<<< Level: %d", key, ce.leafCellType, ce.level)
    }
    kss.ksl.Errorf("[DEBUG FINAL] BUILT CELL ELEMENTS END")
	kss.sortGPUPriority()

	return cellElements
}

func (kss *XPUShareScheduler) sortGPUPriority() {
	models := make([]string, 0, len(kss.gpuPriority))
	prioriries := kss.gpuPriority
	for model := range prioriries {
		models = append(models, model)
	}

	sort.SliceStable(models, func(i, j int) bool {
		return prioriries[models[i]] > prioriries[models[j]]
	})
	kss.sortGPUByPriority = models

	for _, model := range kss.sortGPUByPriority {
		kss.ksl.Infof("GPU priority: %v\t%v", model, kss.gpuPriority[model])
	}
}
/*
func (kss *XPUShareScheduler) addCell(
	cellType string,
	cellTypes map[string]CellTypeSpec,
	cellElements map[string]*cellElement,
	priority int32) {
	kss.ksl.Debugf("[addCell] %v", cellType)
	// check whether the celltype has added in cellElements
	_, ok := cellElements[cellType]
	// already added
	if ok {
		return
	}
	cts, ok := cellTypes[cellType]
	// not found in raw spec, it's leaf cell
	if !ok {
		cellElements[cellType] = &cellElement{
			cellType:        cellType,
			level:           lowestLevel,
			priority:        priority,
			childCellType:   "",
			childCellNumber: 0.0,
			leafCellType:    cellType,
			leafCellNumber:  1.0,
			isNode:          false,
			isMultiNodes:    false,
		}

		// kss.gpuPriorityMutex.Lock()
		// defer kss.gpuPriorityMutex.Unlock()
		kss.gpuPriority[cellType] = priority
		return
	}

	// recursively add children
	child := cts.ChildCellType
	priority = cts.ChildCellPriority

	if _, ok := cellElements[child]; !ok {
		kss.addCell(child, cellTypes, cellElements, priority)
	}

	// child cell type has been added, add current element
	childCellElement := cellElements[child]
	cellElements[cellType] = &cellElement{
		cellType:        cellType,
		level:           childCellElement.level + 1,
		priority:        childCellElement.priority,
		childCellType:   childCellElement.cellType,
		childCellNumber: float64(cts.ChildCellNumber),
		leafCellType:    childCellElement.leafCellType,
		leafCellNumber:  childCellElement.leafCellNumber * float64(cts.ChildCellNumber),
		isNode:          cts.IsNodeLevel,
		isMultiNodes:    childCellElement.isNode || childCellElement.isMultiNodes,
	}
}
	*/

func (kss *XPUShareScheduler) addCell(
	cellType string,
	cellTypes map[string]CellTypeSpec,
	cellElements map[string]*cellElement,
	priority int32) {
	
	// 🔴 修复 1: 清理当前 cellType
    cleanedCellType := strings.TrimSpace(cellType)
    kss.ksl.Debugf("[addCell] %v (Cleaned: %v)", cellType, cleanedCellType)

	// check whether the celltype has added in cellElements
	_, ok := cellElements[cleanedCellType]
	// already added
	if ok {
		return
	}
    
    // 使用清理后的键进行查找
	cts, ok := cellTypes[cleanedCellType] 
	
	// not found in raw spec, it's leaf cell
	if !ok {
		cellElements[cleanedCellType] = &cellElement{
			cellType:        cleanedCellType, // 使用清理后的键
			level:           lowestLevel,
			priority:        priority,
			childCellType:   "",
			childCellNumber: 0.0,
			leafCellType:    cleanedCellType, // 使用清理后的键
			leafCellNumber:  1.0,
			isNode:          false,
			isMultiNodes:    false,
		}

		kss.gpuPriority[cleanedCellType] = priority
		return
	}

	// recursively add children
    // 🔴 修复 2: 清理子 Cell Type
	child := strings.TrimSpace(cts.ChildCellType) 
	priority = cts.ChildCellPriority

	if _, ok := cellElements[child]; !ok {
		kss.addCell(child, cellTypes, cellElements, priority)
	}

	// child cell type has been added, add current element
	childCellElement := cellElements[child]
	cellElements[cleanedCellType] = &cellElement{
		cellType:        cleanedCellType, // 使用清理后的键
		level:           childCellElement.level + 1,
		priority:        childCellElement.priority,
		childCellType:   childCellElement.cellType,
		childCellNumber: float64(cts.ChildCellNumber),
		leafCellType:    childCellElement.leafCellType,
		leafCellNumber:  childCellElement.leafCellNumber * float64(cts.ChildCellNumber),
		isNode:          cts.IsNodeLevel,
		isMultiNodes:    childCellElement.isNode || childCellElement.isMultiNodes,
	}

}

type Cell struct {
	cellType       string
	id             string
	level          int
	higherThanNode bool // true if the cell is higher than node level
	isNode         bool
	priority       int32
	uuid           string
	deviceIndex    int

	leafCellType       string
	leafCellNumber     float64
	availableWholeCell float64 // a number of whole cells are available
	freeMemory         int64
	fullMemory         int64
	available          float64 // remaining gpu resource
	node               string

	healthy bool
	state   CellState

	parent *Cell    // pointer to its parent cell
	child  CellList // pointer to its children cells
}

func NewCell(
	cellType string,
	id string,
	level int,
	higherThanNode bool,
	isNode bool,
	leafCellNumber float64,
	priority int32,
	leafCellType string,

) *Cell {
	return &Cell{
		cellType:           cellType,
		id:                 id,
		level:              level,
		higherThanNode:     higherThanNode,
		isNode:             isNode,
		priority:           priority,
		uuid:               "",
		deviceIndex:        -1,
		freeMemory:         0,
		fullMemory:         0,
		available:          leafCellNumber,
		availableWholeCell: leafCellNumber,
		leafCellType:       leafCellType,
		leafCellNumber:     leafCellNumber,
		healthy:            false,
		state:              CellFree,
	}
}

func NewCellList(top int) LevelCellList {
	ccl := LevelCellList{}
	for i := lowestLevel; i <= top; i++ {
		ccl[i] = CellList{}
	}
	return ccl
}

type cellConstructor struct {
	// input
	cellElements map[string]*cellElement
	cells        []CellSpec

	// output
	cellFreeList map[string]LevelCellList

	// logger
	ksl *logrus.Logger
}

func newCellConstructor(cellElements map[string]*cellElement, cells []CellSpec, ksl *logrus.Logger) *cellConstructor {
	return &cellConstructor{
		cellElements: cellElements,
		cells:        cells,
		cellFreeList: map[string]LevelCellList{},
		ksl:          ksl,
	}
}

func (c *cellConstructor) build() (cellFreeList map[string]LevelCellList) {
	for _, spec := range c.cells {

		rootCell := c.buildFullTree(spec.CellType, spec)

		// --- 容错检查 (来自之前分析) ---
        if rootCell == nil {
            c.ksl.Errorf("[FATAL BUILD] Skipping CellSpec %v due to nil rootCell.", spec.CellType)
            continue
        }
        // --- End error checking ---

		cellType := rootCell.leafCellType
		cellLevel := rootCell.level

		


		// initialize the cell free list
		if _, ok := c.cellFreeList[cellType]; !ok {
			c.cellFreeList[cellType] = NewCellList(cellLevel)
		}
		c.cellFreeList[cellType][cellLevel] = append(
			c.cellFreeList[cellType][cellLevel], rootCell)
	}

	return c.cellFreeList
}

/*
func (c *cellConstructor) buildFullTree(buildingType string, buildingSpec CellSpec) *Cell {



	//check the cellElement of the cellType
	ce, ok := c.cellElements[buildingType]

	if !ok {
        c.ksl.Errorf("cellType %v in Cells is not found in cell types definition (Fatal)", buildingType)
        return nil // 🔴 校验失败，安全退出
    }
    
    if !(ce.isNode || ce.isMultiNodes) {
        c.ksl.Errorf("top cell must be node-level or above: %v (Fatal)", buildingType)
        return nil // 🔴 校验失败，安全退出
    }

	cellInstance := c.buildChildCell(buildingSpec, buildingType, "")
	cellInstance.leafCellType = ce.leafCellType

	

	c.ksl.Errorf("[DEBUG TREE] [BUILD] Root Cell: %s, Level: %d, Configured Children: %d",
		cellInstance.id, cellInstance.level, len(cellInstance.child))

	return cellInstance
}
	*/

func (c *cellConstructor) buildFullTree(buildingType string, buildingSpec CellSpec) *Cell {

	// 1. 检查 cellElement 是否存在
	ce, ok := c.cellElements[buildingType]
	if !ok {
		// 🔴 修复 1: 找不到配置时，打印致命错误并返回 nil
		c.ksl.Errorf("[FATAL CHECK] 1/2. CellType %v in Cells is NOT found in built cellElements. Returning nil.", buildingType)
		return nil 
	}
    
	// 2. 检查是否为 Node 级别
	// make sure cells in xpushare-config.yaml will start from node or above
	if !(ce.isNode || ce.isMultiNodes) {
		// 🔴 修复 2: Node Level 检查失败时，打印致命错误并返回 nil
		c.ksl.Errorf("[FATAL CHECK] 2/2. Node Level Check FAILED for %v. isNode(%v) and isMultiNodes(%v) are both false. Returning nil.", 
            buildingType, ce.isNode, ce.isMultiNodes)
		return nil 
	}

    // 校验成功，继续递归构建子 Cell
	cellInstance := c.buildChildCell(buildingSpec, buildingType, "")
    
    // 3. 检查递归构建是否成功
    if cellInstance == nil {
        c.ksl.Errorf("[FATAL CHECK] buildChildCell returned nil unexpectedly during recursion for %v. Returning nil.", buildingType)
        return nil
    }
    
	cellInstance.leafCellType = ce.leafCellType


	return cellInstance
}

func (c *cellConstructor) buildChildCell(
	spec CellSpec,
	cellType string,
	currentNode string) *Cell {

	ce := c.cellElements[cellType]
	// node-level: pass node name it to its child
	if ce.isNode {
		splitID := strings.Split(string(spec.CellID), "/")
		currentNode = splitID[len(splitID)-1]
	}
	cellInstance := NewCell(cellType, spec.CellID, ce.level, ce.isMultiNodes, ce.isNode, ce.leafCellNumber, ce.priority, ce.leafCellType)
	if !ce.isMultiNodes {
		cellInstance.node = currentNode
	}

	if ce.level == 1 {
		
		c.ksl.Debugf("%+v", cellInstance)
		return cellInstance
	}

	var currentCellChildren CellList
	for _, childSpec := range spec.CellChildren {
		childCellInstance := c.buildChildCell(childSpec, ce.childCellType, currentNode)
		childCellInstance.parent = cellInstance
		if !ce.isMultiNodes {
			childCellInstance.node = currentNode
		}
		currentCellChildren = append(currentCellChildren, childCellInstance)
		// c.ksl.Debugf("%+v", childCellInstance)
	}

	// update current cell children and resource
	cellInstance.child = currentCellChildren
	c.ksl.Debugf("%+v", cellInstance)

	return cellInstance
}
