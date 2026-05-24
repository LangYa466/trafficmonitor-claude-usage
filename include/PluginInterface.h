/*********************************************************
* TrafficMonitor 插件接口
* Copyright (C) by Zhong Yang 2021
* zhongyang219@hotmail.com
**********************************************************/
#pragma once

//插件显示项目的接口
class IPluginItem
{
public:
    /**
     * @brief   获取显示项目的名称
     * @return  const wchar_t*
     */
    virtual const wchar_t* GetItemName() const = 0;

    /**
     * @brief   获取显示项目的唯一ID
     * @return  const wchar_t*
     */
    virtual const wchar_t* GetItemId() const = 0;

    /**
     * @brief   获取项目标签的文本
     * @return  const wchar_t*
     */
    virtual const wchar_t* GetItemLableText() const = 0;

    /**
     * @brief   获取项目数值的文本
     * @detail  由于此函数可能会被频繁调用，因此不要在这里获取监控数据，
     *  而是在ITMPlugin::DataRequired函数中获取数据后保存起来，然后在这里返回获取的数值
     * @return  const wchar_t*
     */
    virtual const wchar_t* GetItemValueText() const = 0;

    /**
     * @brief   获取项目数值的示例文本
     * @detail  此函数返回的字符串的长度会用于计算显示区域的宽度
     * @return  const wchar_t*
     */
    virtual const wchar_t* GetItemValueSampleText() const = 0;

    /**
     * @brief   显示区域是否由插件自行绘制
     * @return  bool
     */
    virtual bool IsCustomDraw() const { return false; }

    /**
     * @brief   获取显示区域的宽度
     * @return  int
     */
    virtual int GetItemWidth() const { return 0; }

    /**
     * @brief   自定义绘制显示区域的函数，只有当CustomDraw()函数返回true时重写此函数才有效
     * @return  void
     */
    virtual void DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode) {}

    /**
     * @brief   获取显示区域的宽度
     * @return  int
     */
    virtual int GetItemWidthEx(void* hDC) const { return 0; }

    /** 鼠标事件的类型 */
    enum MouseEventType
    {
        MT_LCLICKED,    /**< 点击了鼠标左键 */
        MT_RCLICKED,    /**< 点击了鼠标右键 */
        MT_DBCLICKED,   /**< 双击了鼠标左键 */
        MT_WHEEL_UP,    /**< 向上滚动了鼠标滚轮 */
        MT_WHEEL_DOWN,  /**< 向下滚动了鼠标滚轮 */
    };

    enum MouseEventFlag
    {
        MF_TASKBAR_WND = 1 << 0,        /**< 是否为任务栏窗口的鼠标事件 */
    };

    /**
     * @brief   当插件显示区域有鼠标事件时由主程序调用
     * @return  int
     */
    virtual int OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag) { return 0; }

    enum KeyboardEventFlag
    {
        KF_TASKBAR_WND = 1 << 0,        /**< 是否为任务栏窗口的键盘事件 */
    };

    /**
     * @brief   当插件显示区域有键盘事件时由主程序调用
     * @return  int
     */
    virtual int OnKeboardEvent(int key, bool ctrl, bool shift, bool alt, void* hWnd, int flag) { return 0; }

    enum ItemInfoType
    {

    };
    //预留的接口
    virtual void* OnItemInfo(ItemInfoType, void* para1, void* para2) { return 0; }

    /**
     * @brief 是否在在任务栏中显示此项目的资源占用图
     * @return 1：显示，0：不显示
     */
    virtual int IsDrawResourceUsageGraph() const { return 0; }

    /**
     * @brief 获取资源占用图的值。当IsDrawResourceUsageGraphType返回值不为0时有效
     * @return float 资源占用图的值，范围为0.0~1.0。
     */
    virtual float GetResourceUsageGraphValue() const { return 0.0; }
};

class ITrafficMonitor;

///////////////////////////////////////////////////////////////////////////////////////////////////////
//插件接口
class ITMPlugin
{
public:
    /**
     * @brief   插件接口的版本
     * @return  int
     */
    virtual int GetAPIVersion() const { return 7; }

    /**
     * @brief   获取插件显示项目的对象
     * @return  IPluginItem*
     */
    virtual IPluginItem* GetItem(int index) = 0;

    /**
     * @brief   主程序会每隔一定时间调用此函数，插件需要在函数里获取一次监控的数据
     */
    virtual void DataRequired() = 0;

    /** 选项设置对话框的返回值 */
    enum OptionReturn
    {
        OR_OPTION_CHANGED,
        OR_OPTION_UNCHANGED,
        OR_OPTION_NOT_PROVIDED
    };

    /**
     * @brief   主程序调用此函数以打开插件的选项设置对话框
     * @return  ITMPlugin::OptionReturn
     */
    virtual OptionReturn ShowOptionsDialog(void* hParent) { return OR_OPTION_NOT_PROVIDED; }

    /** 插件信息的索引 */
    enum PluginInfoIndex
    {
        TMI_NAME,           /**< 名称 */
        TMI_DESCRIPTION,    /**< 描述 */
        TMI_AUTHOR,         /**< 作者 */
        TMI_COPYRIGHT,      /**< 版权 */
        TMI_VERSION,        /**< 版本 */
        TMI_URL,            /**< 主页 */
        TMI_MAX             /**< 插件信息的最大值 */
    };

    /**
     * @brief   获取此插件的信息，根据index的值返回对应的信息
     */
    virtual const wchar_t* GetInfo(PluginInfoIndex index) = 0;

    /** 主程序的监控信息 */
    struct MonitorInfo
    {
        unsigned long long up_speed{};
        unsigned long long down_speed{};
        int cpu_usage{};
        int memory_usage{};
        int gpu_usage{};
        int hdd_usage{};
        int cpu_temperature{};
        int gpu_temperature{};
        int hdd_temperature{};
        int main_board_temperature{};
        int cpu_freq{};
    };

    /**
     * @brief   主程序调用此函数以向插件传递所有获取到的监控信息
     */
    virtual void OnMonitorInfo(const MonitorInfo& monitor_info) {}

    /**
     * @brief   获取插件要在鼠标提示中显示的文本
     */
    virtual const wchar_t* GetTooltipInfo() { return L""; }

    enum ExtendedInfoIndex
    {
        EI_LABEL_TEXT_COLOR,    //绘图的标签文本颜色
        EI_VALUE_TEXT_COLOR,    //绘图的数值文本颜色
        EI_DRAW_TASKBAR_WND,    //是否绘制任务栏窗口

        //主窗口选项设置
        EI_NAIN_WND_NET_SPEED_SHORT_MODE,
        EI_MAIN_WND_SPERATE_WITH_SPACE,
        EI_MAIN_WND_UNIT_BYTE,
        EI_MAIN_WND_UNIT_SELECT,
        EI_MAIN_WND_NOT_SHOW_UNIT,
        EI_MAIN_WND_NOT_SHOW_PERCENT,

        //任务栏窗口设置
        EI_TASKBAR_WND_NET_SPEED_SHORT_MODE,
        EI_TASKBAR_WND_SPERATE_WITH_SPACE,
        EI_TASKBAR_WND_VALUE_RIGHT_ALIGN,
        EI_TASKBAR_WND_NET_SPEED_WIDTH,
        EI_TASKBAR_WND_UNIT_BYTE,
        EI_TASKBAR_WND_UNIT_SELECT,
        EI_TASKBAR_WND_NOT_SHOW_UNIT,
        EI_TASKBAR_WND_NOT_SHOW_PERCENT,

        EI_CONFIG_DIR,                      //配置文件的目录
    };

    /**
     * @brief   主程序调用此函数以向插件传递更多信息
     */
    virtual void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) {}

    /**
     * @brief   获取插件的图标，HICON格式
     */
    virtual void* GetPluginIcon() { return nullptr; }

    virtual int GetCommandCount() { return 0; }
    virtual const wchar_t* GetCommandName(int command_index) { return nullptr; }
    virtual void* GetCommandIcon(int command_index) { return nullptr; }
    virtual void OnPluginCommand(int command_index, void* hWnd, void* para) {}
    virtual int IsCommandChecked(int command_index) { return false; }

    /**
     * @brief   插件初始化
     */
    virtual void OnInitialize(ITrafficMonitor* pApp) {}
};


///////////////////////////////////////////////////////////////////////////////////////////////////////
//主程序接口
class ITrafficMonitor
{
public:
    virtual int GetAPIVersion() = 0;
    virtual const wchar_t* GetVersion() = 0;

    enum MonitorItem
    {
        MI_UP, MI_DOWN, MI_CPU, MI_MEMORY, MI_GPU_USAGE,
        MI_CPU_TEMP, MI_GPU_TEMP, MI_HDD_TEMP, MI_MAIN_BOARD_TEMP,
        MI_HDD_USAGE, MI_CPU_FREQ, MI_TODAY_UP_TRAFFIC, MI_TODAY_DOWN_TRAFFIC
    };

    virtual double GetMonitorValue(MonitorItem item) = 0;
    virtual const wchar_t* GetMonitorValueString(MonitorItem item, int is_main_window = false) = 0;
    virtual void ShowNotifyMessage(const wchar_t* strMsg) = 0;
    virtual unsigned short GetLanguageId() const = 0;
    virtual const wchar_t* GetPluginConfigDir() const = 0;

    enum DPIType { DPI_MAIN_WND, DPI_TASKBAR };
    virtual int GetDPI(DPIType type) const = 0;
    virtual unsigned int GetThemeColor() const = 0;
    virtual const wchar_t* GetStringRes(const wchar_t* key, const wchar_t* section) = 0;
};


/*
* 注意：插件dll需导出以下函数
* ITMPlugin* TMPluginGetInstance();
*/
