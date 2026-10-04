"""Soviet siege bulldozer definitions; stock armor order is documented beside Verses."""
UNIT_ID = 'SBDOZR'
UNIT_NAME = 'Bulldozer'
ASSETS = ['sbdozr.vxl', 'sbdozr.hva', 'sbdzicon.shp']
SOVIETS = 'Russians,Confederation,Africans,Arabs'
OVERRIDES = {
    'Name': UNIT_NAME, 'UIName': f'Name:{UNIT_ID}', 'Image': UNIT_ID,
    'Prerequisite': 'NAWEAP,NARADR', 'Owner': SOVIETS, 'RequiredHouses': SOVIETS,
    'Primary': 'DozerBlade', 'ElitePrimary': 'DozerBladeE',
    # Immune to mind control, so it can walk up to Psychic Towers and level them; cheaper and
    # tougher for that job, and slower to balance it (2026-10-04: was 2000, 1800 HP, speed 3)
    'Turret': 'no', 'Strength': '2400', 'Armor': 'heavy', 'Speed': '2', 'ImmuneToPsionics': 'yes',
    'ROT': '5', 'Sight': '6', 'TechLevel': '5', 'Cost': '1500', 'Soylent': '1500',
    'Points': '50', 'Weight': '6', 'Size': '6', 'Crusher': 'yes',
    'CrateGoodie': 'no', 'AllowedToStartInMultiplayer': 'no',
    'OpportunityFire': 'no', 'BuildTimeMultiplier': '1.0', 'ThreatPosed': '60',
    'DamageSmokeOffset': '-50,0,150',
}
SECTIONS = '''
; ===== Soviet Bulldozer: close-range demolition blade =====
[DozerBlade]
Damage=600
ROF=45
Range=1.5
CellRangefinding=yes
Projectile=InvisibleLow
Speed=100
Warhead=DozerCrush
Report=TankCrush

[DozerBladeE]
Damage=900
ROF=35
Range=1.5
CellRangefinding=yes
Projectile=InvisibleLow
Speed=100
Warhead=DozerCrush
Report=TankCrush

[DozerCrush]
; none, flak, plate | light, medium, heavy | wood, steel, concrete | special_1, special_2
Verses=200%,200%,200%,5%,5%,5%,200%,200%,200%,5%,5%
CellSpread=0
ProneDamage=100%
InfDeath=1
Wall=yes
Wood=yes
AnimList=S_CLSN16
'''
ART = '''
[SBDOZR]
Voxel=yes
Remapable=yes
Cameo=SBDZICON
AltCameo=SBDZICON
PrimaryFireFLH=180,0,35
ElitePrimaryFireFLH=180,0,35
'''

# AI: two bulldozers with four Rhino escorts, one active team per Soviet house, on every difficulty.
# Clone the stock Grizzly building-assault team: its House=<none> is side-neutral.
AI_TASKFORCE = '0F1BD800-G'
AI_TEAM = '0F1BD810-G'
AI_TRIGGER = '0F1BD820-G'
AI_TEAM_TEMPLATE = '0A86A48C-G'
AI_SCRIPT = '0CE4EC8C-G'  # stock General Attack Buildings (gather, then attack structures)
# Owner owns >= 1 Radar; engine production still requires the unit's War Factory.
# Side 2 = Soviet; easy/medium/hard all enabled; no secondary team.
AI_TRIGGER_LINE = (
    f'{AI_TRIGGER}=Soviet Bulldozer Assault,{AI_TEAM},<all>,5,1,NARADR,'
    '0100000003000000000000000000000000000000000000000000000000000000,'
    '200.000000,50.000000,300.000000,1,0,2,0,<none>,1,1,1'
)
