import json,os,itertools,collections
import sys
# usage: python3 <script> <base worlds dir, e.g. ~/.cache/gothic-ai/base/<fingerprint>/worlds>
B=os.path.expanduser(sys.argv[1])
W=['NEWWORLD','OLDWORLD','ADDONWORLD','DRAGONISLAND']
def walk(vs,out):
    for v in vs:
        out.append(v); walk(v.get('children') or [],out)
want={'START','START_OW_ORETRAIL','START_OW_ABANDONEDMINE','START_NW_ORETRAIL_OW','START_NW_ABANDONEDMINE_OW','START_ADDON','EXIT_ADDON','START_DRAGONISLAND','NW_2_OW_ORETRAIL','OW_2_NW_ORETRAIL'}
logic={}
for w in W:
    d=json.load(open(f'{B}/{w}/vobs.json'))
    allv=[];walk(d['vobs'],allv)
    for v in allv:
        n=(v.get('vob_name') or '').upper()
        if n in want or n.startswith('START') or 'EXIT' in n:
            bb=v['bbox']
            print(w,v['class'],n,[round(x/100,1) for x in v['position']],'bbox',[round(x/100) for x in bb['min']],[round(x/100) for x in bb['max']])
    logic[w]=set((v.get('vob_name') or '').upper() for v in allv if v['class'] in ('zCMover','zCTrigger','zCTriggerList','oCTriggerScript','zCMessageFilter','zCCodeMaster','oCMobSwitch','oCMobDoor','oCMobContainer','oCMobInter','oCMOB','oCMobBed','oCMobFire','zCTriggerWorldStart','oCTouchDamage','zCPFXController','zCCSCamera') and v.get('vob_name'))
for a,b in itertools.combinations(W,2):
    s=logic[a]&logic[b]; print('logic collide',a,b,len(s),sorted(s)[:25])
