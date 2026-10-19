<?xml version="1.0"?>
<!-- sxml transform: xsl:number with lang over the CLDR spell-out rules
     the command embeds (D221): German words and ordinals by an ending,
     Italian by a rule set, French ordinal suffixes of digits, English. -->
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="text"/>
  <xsl:template match="/">
    <xsl:number value="134" format="Ww" lang="de"/>
    <xsl:text>&#10;</xsl:text>
    <xsl:number value="10" format="w" lang="de" ordinal="-er"/>
    <xsl:text>&#10;</xsl:text>
    <xsl:number value="5" format="w" lang="it" ordinal="%spellout-ordinal-feminine"/>
    <xsl:text>&#10;</xsl:text>
    <xsl:number value="1" format="1" lang="fr" ordinal="yes"/>
    <xsl:text>&#10;</xsl:text>
    <xsl:number value="22" format="Ww" lang="en"/>
    <xsl:text>&#10;</xsl:text>
  </xsl:template>
</xsl:stylesheet>
