<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="text"/>

  <xsl:template match="/">
    <xsl:apply-templates select="//middle//(section | t[@anchor])"/>
  </xsl:template>

  <!-- 1, 1.1, 2, ...: the position among the sections at each level. -->
  <xsl:template match="section">
    <xsl:number level="multiple" count="section" format="1.1"/>
    <xsl:text>  {name}&#10;</xsl:text>
  </xsl:template>

  <!-- R1, R2, ...: the position among all requirements of the draft. -->
  <xsl:template match="t">
    <xsl:text>    R</xsl:text>
    <xsl:number level="any" count="t[@anchor]" format="1"/>
    <xsl:text> {@anchor}: {bcp14}&#10;</xsl:text>
  </xsl:template>
</xsl:stylesheet>
