<?xml version="1.0"?>
<!-- sxml transform: template rules, a stylesheet parameter, sorting, a
     key, xsl:number, and an included module (docs/CLI.md). -->
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:x="urn:x" exclude-result-prefixes="x">
  <xsl:include href="common.xsl"/>
  <xsl:param name="heading" select="'Books'"/>
  <xsl:output method="xml" indent="yes" omit-xml-declaration="yes"/>
  <xsl:key name="by-id" match="book" use="@id"/>
  <xsl:template match="/catalog">
    <list heading="{$heading}" first="{key('by-id', 'b1')/title}">
      <xsl:apply-templates select="book">
        <xsl:sort select="x:price" data-type="number" order="descending"/>
      </xsl:apply-templates>
    </list>
  </xsl:template>
  <xsl:template match="book">
    <item n="{position()}"><xsl:call-template name="label"/></item>
  </xsl:template>
</xsl:stylesheet>
