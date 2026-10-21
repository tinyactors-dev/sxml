<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="text"/>

  <!-- Every element with an anchor, found by its anchor. -->
  <xsl:key name="anchor" match="*[@anchor]" use="@anchor"/>

  <xsl:template match="/">
    <xsl:apply-templates select="//xref"/>
  </xsl:template>

  <xsl:template match="xref">
    <xsl:text>{@target} -> </xsl:text>
    <xsl:apply-templates select="key('anchor', @target)" mode="xref-text"/>
    <xsl:text>&#10;</xsl:text>
  </xsl:template>

  <!-- The text of an xref depends on what it points to. -->
  <xsl:template match="section" mode="xref-text">
    <xsl:text>Section </xsl:text>
    <xsl:number level="multiple" count="section" format="1.1"/>
  </xsl:template>

  <xsl:template match="reference" mode="xref-text">[{@anchor}]</xsl:template>

  <xsl:template match="dt" mode="xref-text">the term "{.}"</xsl:template>
</xsl:stylesheet>
