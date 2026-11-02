import { Button } from '@/components/ui/button'
import { usePublicLocale } from '../locale'

export function BatchActions({ desktop = false }: { desktop?: boolean }) {
  const { t } = usePublicLocale()
  return (<div className="batch-actions" role="group" aria-label={desktop ? "列表内设备操作" : t("所选设备操作")}>
          <Button variant="ghost" type="button" data-operation="status">
            {desktop ? "刷新状态" : t("同步状态")}
          </Button>
          <Button variant="ghost" type="button" data-operation="rebootLoader">
            {t("切换至 Loader")}
          </Button>
          <Button variant="ghost" type="button" data-operation="rebootApp">
            {t("切换至 App")}
          </Button>
          <Button variant="ghost" type="button" data-operation="restart">
            {t("重启")}
          </Button>
          <Button variant="ghost" type="button" data-operation="stage">
            {t("发送固件包")}
          </Button>
          <Button variant="ghost" type="button" data-operation="install" className="batch-primary">
            {t("发送并切换")}
          </Button>
          <Button variant="ghost" type="button" id="cancel-all">
            {t("取消任务")}
          </Button>
        </div>)
}

export function BatchTable({ desktop = false }: { desktop?: boolean }) {
  const { t } = usePublicLocale()
  return (<div className="batch-table-scroll">
          <table className="batch-table">
            <thead>
              <tr>
                {!desktop && <th>
                  <input type="checkbox" id="select-all" aria-label={t("选择全部设备")} />
                </th>}
                <th>
                  {t("设备 / 端口")}
                </th>
                <th>
                  {t("板型 / Target")}
                </th>
                <th>
                  {t("当前镜像")}
                </th>
                <th>
                  {t("状态")}
                </th>
                <th>
                  {t("可用命令")}
                </th>
                <th>
                  {t("进度 / 详情")}
                </th>
                <th>
                  {t("操作")}
                </th>
              </tr>
            </thead>
            <tbody id="device-rows">
              <tr>
                <td colSpan={desktop ? 7 : 8} className="batch-empty">
                  {t(desktop ? "点击“扫描串口”读取设备。" : "正在读取已授权端口…")}
                </td>
              </tr>
            </tbody>
          </table>
        </div>)
}
