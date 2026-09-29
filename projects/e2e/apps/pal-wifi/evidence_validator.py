import hashlib,json,re
from pathlib import Path

def markers(path):
    text=Path(path).read_text(errors='replace')
    text=re.sub(r'\x1b\[[0-9;]*m','',text)
    result={}
    for match in re.finditer(r'(H2_WIFI_[A-Z_]+) (\{[^\n]+\})',text):
        try:value=json.loads(match[2])
        except ValueError:continue
        result.setdefault(match[1],[]).append(value)
    return result

def verify_boot(path,ids,board,version,seed=False):
    m=markers(path)
    assert len(m.get('H2_WIFI_BOOT',[]))==1,'missing or repeated boot'
    boot=m['H2_WIFI_BOOT'][0]; key=(boot['boot'],boot['nonce'])
    assert boot['board']==board and boot['version']==version
    cases=m.get('H2_WIFI_CASE',[])
    assert [x['id'] for x in cases]==['settings-restart-persistence']+ids,'wrong ordered registry'
    assert all((x['boot'],x['nonce'])==key for x in cases),'mixed boot/replayed ledger'
    assert all(x['status']=='PASS' and x['rc']==0 for x in cases[1:]),'mandatory failure or blocked'
    assert cases[0]['status']==('PENDING' if seed else 'PASS') and cases[0]['rc']==(-9 if seed else 0)
    rows={}
    for tag in ('REPORT','RESTORE','STA_EVENTS','AP_EVENTS','CLIENT'):
        values=m.get('H2_WIFI_'+tag,[]);assert len(values)==1,(tag,len(values))
        row=values[0];assert (row['boot'],row['nonce'])==key;rows[tag]=row
    r=rows['REPORT'];assert r['board']==board and r['version']==version and r['operations']==21
    assert r['passed']==len(ids)+(not seed) and not r['failed'] and not r['blocked']
    assert r['persistence']==(not seed) and r['qualified']==(not seed) and r['rc']==(-9 if seed else 0)
    r=rows['RESTORE'];assert r['cleanup']==0 and r['saved_restored']==1 and r['network_restored']==1 and r['backup_cleared']==1 and r['retained']==0
    r=rows['STA_EVENTS'];assert r['invalid']==0 and all(r[x]>0 for x in ('connecting','connected','got_ip','disconnected','route_changed'))
    r=rows['AP_EVENTS'];assert all(r[x]>0 for x in ('started','stopped','joined','left'))
    text=Path(path).read_text(errors='replace')
    assert ('H2_WIFI_READY board='+board+' rc='+('-9' if seed else '0')+' confirm='+('-7' if seed else '0')) in text
    assert 'Task watchdog got triggered' not in text and 'panic' not in text.lower()
    return {'boot':boot,'cases':cases,'report':rows,'log_sha256':hashlib.sha256(Path(path).read_bytes()).hexdigest()}
